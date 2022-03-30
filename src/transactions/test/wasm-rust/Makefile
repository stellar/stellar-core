.PHONY: build

build: test_invoke_contract.wasm test_typed_contract.wasm test_other_contract.wasm


STRIP=symbols
#STRIP=debuginfo

%.wasm: %.rs Makefile $(wildcard src/*.rs)
	rustc --edition 2021 --crate-type cdylib --target wasm32-unknown-unknown -C opt-level=z -C panic=abort -C overflow-checks=no -C strip=$(STRIP)  $<

clean:
	rm -f *.wasm
