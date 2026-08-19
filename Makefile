.PHONY: shell build replay dev-clean dev-image

shell:
	@bash scripts/dev_container.sh shell

build:
	@bash scripts/dev_container.sh build

replay:
	@bash scripts/dev_container.sh replay

dev-clean:
	@bash scripts/dev_container.sh clean

dev-image:
	@bash scripts/dev_container.sh image
