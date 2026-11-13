CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra

pman: pman.c
	$(CC) $(CFLAGS) -o pman pman.c

clean:
	rm -f pman

install: pman
	install -m 0755 pman /usr/local/bin/pman

git:
	@printf "Commit message: "; \
  read msg; \
  test -n "$$msg" || { echo "Commit message is required"; exit 1; }; \
  git add -A; \
  git commit -m "$$msg"

.PHONY: clean install git
