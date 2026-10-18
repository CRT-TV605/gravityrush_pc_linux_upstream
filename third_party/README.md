# Third-party code

LibAtrac9 (submodule, MIT, https://github.com/Thealexbarney/LibAtrac9) decodes the ATRAC9
audio of the game's music and streamed sounds (src/gr_ngs2_voice.inc). `git submodule update --init`
fetches it; build.sh compiles it into out/libatrac9.a.
