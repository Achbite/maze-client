# Maze Contract Snapshot

This directory owns the Maze protocol sources and generated C++ bindings used
by this Client checkout. Maze Client always compiles these repository-local
files and never discovers or mounts an external Contracts artifact.

`manifest.json` records only the local task protocol identity and required build
inputs. Run `../scripts/sync_contract_snapshot.sh` only when you explicitly
choose to replace the checkout with the Maze release selected in
`artifact_versions.env`. Normal builds and `make shell` never run that command,
and no source, generator, hash, or platform equality gates Client/AIServer
communication.
