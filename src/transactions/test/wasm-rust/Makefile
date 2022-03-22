.PHONY: build

build: test_invoke_contract.wasm

%.wasm: %.rs Makefile
	rustc --crate-type cdylib --target wasm32-unknown-unknown -C opt-level=z -C panic=abort -C overflow-checks=no -C strip=symbols $<

clean:
	rm -f *.wasm
