OSCQuery Automation @VERSION@, Linux @ARCH@

DAW automation lanes bound to any parameter an OSCQuery device publishes.
VST3 and CLAP.

Install
    ./install.sh --user      ~/.vst3 and ~/.clap, no sudo
    ./install.sh --system    /usr/local/lib/vst3 and /usr/lib/clap, with sudo

Uninstall
    ./uninstall.sh --user    (or --system, as installed)

Then rescan plugins in the host. The plugins appear under the manufacturer
"Pix et Bel". Your settings file is not touched by either script.

Hosts that load these plugins on Linux include REAPER and Bitwig Studio.

Licence: GNU GPL v3 or later (Docs/LICENSE.txt); third-party notices in
Docs/THIRD_PARTY_NOTICES.txt.
Source and issues: https://github.com/pob31/OSCQuery-Automation-plugin
