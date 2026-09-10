# Maze Protocol Snapshot

This directory owns the Maze protocol sources and generated C++ bindings used
by this Client checkout. Maze Client always compiles these repository-local
files and never discovers or mounts an external Contracts artifact.

The snapshot contains `common/identity`, `communication/session`, and
`maze/maze` sources and C++ bindings, plus the `rl_sdk/` headers. It does
not include Learner, Sample Pool, Model Distributor or metric transport bindings.
Generated includes use `proto/...` paths from the repository root.

Run `bash ../scripts/sync_contract_snapshot.sh` only when you explicitly choose
to replace these files with the current Maze protocol from the sibling
`rl-contracts` checkout. Normal builds and `make shell` never run that command.
Client/AIServer communication is determined by protobuf wire fields, with no
protocol ID, source hash, package version, generator, or platform equality gate.
