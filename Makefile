.PHONY: shell build test verify replay dev-clean dev-image

shell:
	@bash scripts/dev_container.sh shell

build:
	@bash scripts/dev_container.sh build

test:
	@bash scripts/dev_container.sh test

verify:
	@bash scripts/dev_container.sh verify

replay:
	@bash scripts/dev_container.sh replay

dev-clean:
	@bash scripts/dev_container.sh clean

dev-image:
	@bash scripts/dev_container.sh image
