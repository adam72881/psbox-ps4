# legacy/

Frozen copy of the pre-01.57 PSBox C++/CMake tree (before the XCloud4 base swap).

Still to port onto the XCloud4 C client (when needed):

- PSCloud/PSBox C++ UI (library chrome, settings screens) — `legacy/psbox/src/ui`, `src/app`
- GoldHEN Remote Package Install update path — `legacy/psbox/src/app/local_update.*`, payload under `src/payload`

Do **not** merge streaming/WebRTC from here: stream transport is XCloud4 only.
