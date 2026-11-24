# Security

Report a vulnerability privately through GitHub's private vulnerability reporting, at
<https://github.com/ciphernaut/onair-rx/security/advisories/new> (the same form as **Security**,
then **Report a vulnerability**, on the repository page; GitHub asks you to sign in). Only the
maintainer sees the report. Please do not open a public issue for it. Say what an attacker can
reach and do, with the commit and the steps to reproduce. This is a one-person project, so allow
a few days for an answer.

`onair-rx` reads untrusted input: IQ samples off the air, and the frames OnAir's receivers decode
from them, which it turns into JSON lines and pcap records. A crash, a hang or a memory error on
any input is in scope, as is the pcap port (it listens on 127.0.0.1 only) accepting more than one
local client or being reachable from elsewhere. A weakness in OnAir itself belongs with
[its authors](https://github.com/adswill/OnAir), unless it is in the patch this repository
carries (`patches/`) or in code copied from OnAir into `src/`.
