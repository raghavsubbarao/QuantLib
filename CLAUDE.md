@AGENTS.md

## Fork-specific notes

This fork adds FX smile-section, local-vol and stochastic-local-vol
(SLV) functionality. All of it lives under `ql/experimental/fxslv/`, the
smile sections in its `smilesections/` subdirectory, with tests in
`test-suite/fxlocalvol.cpp`, `fxslvmodel.cpp` and `fxsmilesections.cpp`,
and examples in `Examples/FXLocalVol` and `Examples/FXSmileSections`.

When adding a file to the fork, keep it under `ql/experimental/fxslv/`
and register it in `ql/CMakeLists.txt`, the `Makefile.am` and `all.hpp`
of its directory, `QuantLib.vcxproj` and `QuantLib.vcxproj.filters`
(under the filter of its directory). A new subdirectory also needs its
own `Makefile.am` and `all.hpp` (on the pattern of `smilesections/`), an
entry in the parent's `SUBDIRS` and `libFxSlv_la_LIBADD`, an include of
its `all.hpp` in the parent's, its `all.hpp` in `QuantLib.vcxproj*` and
in the generated headers of `ql/CMakeLists.txt` (`${PROJECT_BINARY_DIR}/...`),
a `<Filter>` in `QuantLib.vcxproj.filters` and its `Makefile` in
`configure.ac` next to `ql/experimental/fxslv/Makefile`;
`tools/check_filelists.sh` checks the lists.
That one line in `configure.ac` is the only upstream file list to edit;
avoid editing the others, so that merges from lballabio/QuantLib stay
conflict-free.
