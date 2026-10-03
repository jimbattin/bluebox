BUN ?= bun
PORT ?= 3000
STAMP = node_modules/.installed

$(STAMP): package.json bun.lock
	$(BUN) install --frozen-lockfile
	touch $@

serve: $(STAMP)
	PORT=$(PORT) $(BUN) run server.ts

html: dist/editor.html

dist/editor.html: $(STAMP) build.ts $(shell find src -type f)
	$(BUN) run build.ts

test: $(STAMP)
	$(BUN) test

check: $(STAMP)
	$(BUN) x tsc --noEmit
	$(BUN) x prettier --check .

fmt: $(STAMP)
	$(BUN) x prettier --write .

clean:
	rm -rf dist

.PHONY: serve html test check fmt clean
