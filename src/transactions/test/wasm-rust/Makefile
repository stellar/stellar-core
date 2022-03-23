.PHONY: build

build: test_invoke_contract.wasm test_typed_contract.wasm


%.wasm: %.rs Makefile $(wildcard src/*.rs)
	rustc --edition 2021 --crate-type cdylib --target wasm32-unknown-unknown -C opt-level=z -C panic=abort -C overflow-checks=no -C strip=symbols  $<

clean:
	rm -f *.wasm
