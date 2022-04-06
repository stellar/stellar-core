all:
	cargo build --target wasm32-unknown-unknown --release
	for i in target/wasm32-unknown-unknown/release/*.wasm ; do \
		wasm-opt -Oz "$$i" -o "$$i.tmp" && mv "$$i.tmp" "$$i"; \
	done

clean:
	cargo clean

