# Security

Prismflow can read/write files and make HTTP requests (`file`, `http` modules). In the C library these are
**off by default** and must be enabled by the host application; the playground has them disabled.
Treat untrusted scripts as untrusted code and keep those modules disabled when running them.

Report vulnerabilities privately through GitHub "Security advisories" (Security tab → Report a vulnerability).
