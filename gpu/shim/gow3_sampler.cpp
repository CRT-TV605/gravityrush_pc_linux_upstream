// SPDX-License-Identifier: GPL-2.0-or-later
#include "gow3_sampler.h"

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace Gow3Sampler {
namespace {

constexpr int Depth = 24;

// The stack of a suspended thread: its own frame and up to Depth - 1 callers (x64 unwind data).
int Unwind(CONTEXT ctx, DWORD64* out) {
    int n = 0;
    while (n < Depth && ctx.Rip) {
        out[n++] = ctx.Rip;
        DWORD64 base = 0;
        RUNTIME_FUNCTION* fn = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
        if (!fn) {
            break; // a leaf without unwind data, or code outside any image
        }
        void* handler_data = nullptr;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx, &handler_data, &frame, nullptr);
    }
    return n;
}

// "module!export+offset" for an address in a DLL: the nearest exported function below it.
std::string DescribeOutside(DWORD64 rip) {
    static std::map<DWORD64, std::string> names;
    if (const auto it = names.find(rip); it != names.end()) {
        return it->second;
    }
    HMODULE module = nullptr;
    char text[512] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(rip), &module)) {
        char file[MAX_PATH] = "";
        GetModuleFileNameA(module, file, MAX_PATH);
        const char* name = std::max(std::strrchr(file, '\\'), std::strrchr(file, '/'));
        name = name ? name + 1 : file;
        const auto* base = reinterpret_cast<const BYTE*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        const char* best = nullptr;
        DWORD best_rva = 0;
        const DWORD rva = DWORD(rip - DWORD64(base));
        if (dir.Size) {
            const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
            const auto* functions = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
            const auto* names_rva = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
            const auto* ordinals = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
            for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
                const DWORD f = functions[ordinals[i]];
                if (f <= rva && f >= best_rva) {
                    best_rva = f;
                    best = reinterpret_cast<const char*>(base + names_rva[i]);
                }
            }
        }
        if (best) {
            std::snprintf(text, sizeof(text), "%s!%s+%x", name, best, rva - best_rva);
        } else {
            std::snprintf(text, sizeof(text), "%s+%x", name, rva);
        }
    }
    return names.emplace(rip, text).first->second;
}

void Write(const std::string& path, const std::string& name, DWORD64 module,
           const std::map<std::vector<DWORD64>, unsigned>& stacks, unsigned total) {
    std::vector<std::pair<unsigned, const std::vector<DWORD64>*>> sorted;
    sorted.reserve(stacks.size());
    for (const auto& [stack, count] : stacks) {
        sorted.emplace_back(count, &stack);
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f) {
        return;
    }
    std::fprintf(f, "# thread %s, %u samples; count then frames (offsets from the executable, "
                    "0x... = outside it)\n", name.c_str(), total);
    for (size_t i = 0; i < sorted.size() && i < 4000; ++i) {
        std::fprintf(f, "%u", sorted[i].first);
        for (const DWORD64 rip : *sorted[i].second) {
            const DWORD64 offset = rip - module;
            if (offset < 0x10000000) {
                std::fprintf(f, " +%llx", static_cast<unsigned long long>(offset));
            } else {
                std::fprintf(f, " %s", DescribeOutside(rip).c_str());
            }
        }
        std::fputc('\n', f);
    }
    std::fclose(f);
    MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

} // namespace

void SampleCurrentThread(const char* name) {
    const char* file = std::getenv("GOW3_SAMPLE_FILE");
    if (!file || !*file) {
        return;
    }
    HANDLE target = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                               FALSE, GetCurrentThreadId());
    if (!target) {
        return;
    }
    std::printf("Sampler: %s -> %s-%s.txt\n", name, file, name);
    std::thread([target, path = std::string(file) + "-" + name + ".txt", name = std::string(name)] {
        timeBeginPeriod(1);
        const auto module = reinterpret_cast<DWORD64>(GetModuleHandleW(nullptr));
        std::map<std::vector<DWORD64>, unsigned> stacks;
        unsigned total = 0;
        auto next_write = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        DWORD64 frames[Depth];
        while (true) {
            Sleep(1);
            if (SuspendThread(target) == DWORD(-1)) {
                break; // the thread ended
            }
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            int n = 0;
            if (GetThreadContext(target, &ctx)) {
                n = Unwind(ctx, frames);
            }
            ResumeThread(target);
            if (n) {
                ++stacks[std::vector<DWORD64>(frames, frames + n)];
                ++total;
            }
            if (std::chrono::steady_clock::now() >= next_write) {
                Write(path, name, module, stacks, total);
                next_write += std::chrono::seconds(10);
            }
        }
    }).detach();
}

} // namespace Gow3Sampler

#else

namespace Gow3Sampler {
void SampleCurrentThread(const char*) {}
} // namespace Gow3Sampler

#endif
