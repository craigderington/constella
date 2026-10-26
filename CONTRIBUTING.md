# Contributing

Build and test from a clean checkout using the root README. Keep changes focused
and describe the concrete behavior, validation and limitations. Add regression
coverage for meaningful correctness/security failures. Consensus changes need
an explicit versioned design, vectors and C/Go agreement tests.

Run C tests, the five-profile protocol lab, v5 Explorer tests with PostgreSQL
integration, wallet tests and the release source guard before a release. CI must
qualify both supported Linux architectures. Keep dependencies locked, retain
vendor notices and record immutable build inputs. New tests/documentation must
be tracked and runnable by someone who only has a clone.

Never commit spending keys, env files, live histories, provider credentials,
operator backups, private incident evidence or local agent guidance. Disposable
public fixtures must be identified as such. Use private reporting for exploitable
security findings; see [SECURITY.md](SECURITY.md).

Existing Git history is retained. Do not backdate validation, rewrite published
commits to conceal fixes, or move release tags. Record what a deployment actually
runs, including uncommitted patches, executable hashes, image IDs and mounted UI
files. A source commit alone is insufficient when an image contains different
code or host-mounted files replace embedded presentation assets.
