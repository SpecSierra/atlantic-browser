Atlantic Browser
================
Atlantic Browser is the maintained Sailfish OS browser port in this repository. It keeps the Sailfish Silica Qt browser UI and uses the carried-forward WPE WebKit stack.
More information about the older Sailfish Browser architecture can still be found at https://web.archive.org/web/20180830103541/http://blog.idempotent.info/posts/whats-behind-sailfish-browser.html .

Maintainer
----------
- SpecSierra

Current architecture
--------------------
- Sailfish Silica / Qt browser UI remains in this repository.
- Web content is provided by the carried-forward **WPE WebKit** Qt5 bridge and runtime under `apps/wpe/`.
- Shared browser startup/runtime glue lives under `apps/lib/` and `apps/browser/`.
- Build, packaging, runtime wrapper, and compatibility work now live in the companion repo: https://github.com/SpecSierra/wpe-sfos-build

Tools
-----
The remaining standalone tools are located under [tools](https://github.com/SpecSierra/atlantic-browser/tree/main/tools). They are auxiliary developer utilities, not the main browser build flow.

#### [memory-dump-reader](https://github.com/SpecSierra/atlantic-browser/tree/main/tools/memory-dump-reader)

Memory dump reader is a small desktop utility for dumping and collecting browser memory information from a device. It is useful for debugging/investigation, but it is not part of the normal Atlantic Browser build or packaging path.

##### Compilation

- Change directory to the tools/memory-dump-reader
- \<qmake-bin-path\>/qmake
- make

##### Reading and collecting

Once memory-dump-reader is compiled, run it like: `dumpMemoryInfo /tmp/fileName.gz ip-of-the-device`.
The script collects browser memory information from the device and copies the dump back to the host.
`dumpMemoryInfo` works best when your public SSH key is already authorized on the device.

License
-------
The browser is open source and licensed under Mozilla Public License v2.0 (http://www.mozilla.org/MPL/2.0/).

Repository
----------
GitHub: https://github.com/SpecSierra/atlantic-browser
