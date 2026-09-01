.PHONY: shell build replay replay-stop dev-clean dev-image dev-refresh

shell:
	@bash scripts/dev_container.sh shell

build:
	@bash scripts/dev_container.sh build

replay:
	@bash scripts/dev_container.sh replay

replay-stop:
	@bash scripts/dev_container.sh replay-stop

dev-clean:
	@bash scripts/dev_container.sh clean

dev-image:
	@bash scripts/dev_container.sh image

dev-refresh:
	@bash scripts/dev_container.sh refresh
