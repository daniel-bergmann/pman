# pman

A tiny "man" for PHP functions. Fetches the online [PHP manual](https://www.php.net/)
and shows the function's name, version info, purpose, signature, description,
parameters, return values, code examples, and "see also" links.

## Requirements

- A C compiler (`cc` / `clang` / `gcc`)
- `make`
- `curl` (used for the HTTPS request to php.net)

## Build

```sh
make
```

This produces the `pman` binary in the current directory.

## Usage

```sh
./pman strlen
./pman array_map
./pman 'strlen()'      # trailing () / whitespace is fine
./pman --help
```

Multiple functions can be looked up at once:

```sh
./pman strlen array_map
```

## Pager

When output is a terminal, `pman` opens a built-in pager:

| Key                | Action      |
| ------------------ | ----------- |
| `j` / `↓`          | scroll down |
| `k` / `↑`          | scroll up   |
| `Space` / `PageDown` | page down |
| `PageUp`           | page up     |
| `q`                | quit        |
| `:q`               | quit        |
| `Ctrl-C` / `Ctrl-D` | quit       |

When output is piped or redirected, it prints plain text with no pager and no
ANSI colors.

## Install

Install system-wide to `/usr/local/bin` (requires root):

```sh
sudo make install
```

Then use it from any shell:

```sh
pman strlen
```

Install without root, to a user-writable directory on your `PATH`:

```sh
install -m 0755 pman ~/.local/bin/pman
```

(Check that `~/.local/bin` is on your `PATH` with
`echo "$PATH" | tr ':' '\n' | grep .local/bin`.)

## Clean up

```sh
make clean
```
