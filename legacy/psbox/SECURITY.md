# Security policy

## Reporting a vulnerability

Please **don't open a public issue** for security problems. Report them
privately through GitHub instead:
<https://github.com/adam72881/psbox-ps4/security/advisories/new>.

Include what you found, the release (`v0.1.0`, …) or commit, and how to
reproduce it. I'll reply as soon as I can and credit you in the fix unless you'd
rather stay anonymous.

## Your account

The app stores its Microsoft sign-in (a refresh token) in
`/data/psbox/account.json` on the PS4. Treat that file like a
password: never share it or attach it to an issue. If it may have leaked, sign
out of all sessions at <https://account.microsoft.com/security> and sign in on
the console again. The log (`xcloud.log`) contains no passwords or tokens.

## Scope

In scope: this repository's code (sign-in, streaming, UI, build and packaging
scripts) and the release zips published here.

Out of scope:

- Microsoft's services (Xbox Live, xCloud) and their APIs;
- the PS4/PS5 homebrew environment, OpenOrbis / payload SDK and related tooling;
- third-party libraries (report those upstream; see `THIRD_PARTY_NOTICES.md`).
