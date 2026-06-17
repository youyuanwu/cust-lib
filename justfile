# cust-lib build tasks
# Run `just` or `just --list` to see available recipes.

cust_dir := justfile_directory() / ".cust"
cust := cust_dir / "bin" / "cust"

# Show available recipes
default:
    @just --list

# Install the cust driver + clang plugin into the workspace .cust folder
install-cust:
    mkdir -p {{cust_dir}}/bin
    curl -fsSL https://raw.githubusercontent.com/youyuanwu/cust/main/scripts/install.sh | CUST_INSTALL_DIR={{cust_dir}}/bin bash

# Build the project
build:
    {{cust}} build

# Run the test suite
test:
    {{cust}} test

# Run the project
run:
    {{cust}} run

# Remove the build cache
clean:
    rm -rf target
