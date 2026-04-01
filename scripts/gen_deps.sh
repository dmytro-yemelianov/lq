#!/bin/bash
#
# Generate C dependency graph in SQLite database
# Usage: ./gen_deps.sh [project_dir] [output.db]
#

set -e

# Check for required dependencies
if ! command -v sqlite3 &>/dev/null; then
    echo "Error: sqlite3 is not installed." >&2
    echo "In Debian, you may install it with: apt install sqlite3" >&2
    exit 1
fi

if ! command -v gcc &>/dev/null; then
    echo "Error: gcc is not installed." >&2
    echo "In Debian, you may install it with: apt install gcc" >&2
    exit 1
fi

PROJECT_DIR="${1:-/tmp/aaa/qsoe}"
DB_FILE="${2:-/tmp/aaa/deps.db}"

# Remove old database
rm -f "$DB_FILE"

# Create database schema
sqlite3 "$DB_FILE" <<'EOF'
CREATE TABLE files (
    id INTEGER PRIMARY KEY,
    path TEXT UNIQUE NOT NULL
);

CREATE TABLE dependencies (
    target_id INTEGER NOT NULL REFERENCES files(id),
    dep_id INTEGER NOT NULL REFERENCES files(id),
    PRIMARY KEY (target_id, dep_id)
);

CREATE INDEX idx_dep_target ON dependencies(target_id);
CREATE INDEX idx_dep_dep ON dependencies(dep_id);
EOF

echo "Database created: $DB_FILE"

# Temporary file for batch inserts
TMPFILE=$(mktemp)
trap "rm -f $TMPFILE" EXIT

# Function to get or create file ID
get_file_id() {
    local path="$1"
    sqlite3 "$DB_FILE" "INSERT OR IGNORE INTO files(path) VALUES('$path');"
    sqlite3 "$DB_FILE" "SELECT id FROM files WHERE path='$path';"
}

# Function to process a single .c file
process_file() {
    local src_file="$1"
    shift
    local include_flags="$@"

    # Get dependencies from gcc (ignore errors for missing headers)
    local deps
    deps=$(gcc -MM $include_flags "$src_file" 2>/dev/null | tr -d '\\\n' | sed 's/  */ /g') || return 0

    if [[ -z "$deps" ]]; then
        return 0
    fi

    # Parse: target.o: file1.c file2.h ...
    local target="${deps%%:*}"
    target=$(echo "$target" | xargs)  # trim whitespace
    local dep_list="${deps#*: }"

    # Convert target.o back to source path for clarity
    # We'll use the actual source file as the target
    local target_path="$src_file"

    # Get target ID
    local target_id=$(get_file_id "$target_path")

    # Process each dependency
    for dep in $dep_list; do
        # Normalize path (resolve relative paths)
        local dep_path
        if [[ "$dep" = /* ]]; then
            dep_path="$dep"
        else
            dep_path=$(cd "$(dirname "$src_file")" && realpath -m "$dep" 2>/dev/null || echo "$dep")
        fi

        local dep_id=$(get_file_id "$dep_path")

        # Insert dependency (ignore duplicates)
        sqlite3 "$DB_FILE" "INSERT OR IGNORE INTO dependencies(target_id, dep_id) VALUES($target_id, $dep_id);"
    done
}

# Discover include directories
KERNEL_INCLUDES="-I$PROJECT_DIR/kernel/include -I$PROJECT_DIR/kernel/startup/include"
LIBC_INCLUDES="-I$PROJECT_DIR/userland/libc/include -I$PROJECT_DIR/userland/libc/src/include"
TASKMAN_INCLUDES="-I$PROJECT_DIR/userland/taskman/runenv/include"

echo "Processing kernel sources..."
count=0
while IFS= read -r -d '' src; do
    process_file "$src" $KERNEL_INCLUDES
    ((++count))
    if ((count % 50 == 0)); then
        echo "  processed $count files..."
    fi
done < <(find "$PROJECT_DIR/kernel" -name "*.c" -print0)
echo "  kernel: $count files"

echo "Processing userland/libc sources..."
count=0
while IFS= read -r -d '' src; do
    process_file "$src" $LIBC_INCLUDES
    ((++count))
    if ((count % 100 == 0)); then
        echo "  processed $count files..."
    fi
done < <(find "$PROJECT_DIR/userland/libc" -name "*.c" -print0 2>/dev/null)
echo "  libc: $count files"

echo "Processing userland/taskman sources..."
count=0
while IFS= read -r -d '' src; do
    process_file "$src" $TASKMAN_INCLUDES $LIBC_INCLUDES
    ((++count))
done < <(find "$PROJECT_DIR/userland/taskman" -name "*.c" -print0 2>/dev/null)
echo "  taskman: $count files"

# Print summary
echo ""
echo "=== Summary ==="
sqlite3 "$DB_FILE" "SELECT 'Files: ' || COUNT(*) FROM files;"
sqlite3 "$DB_FILE" "SELECT 'Dependencies: ' || COUNT(*) FROM dependencies;"

echo ""
echo "Done! Database saved to: $DB_FILE"
echo ""
echo "Example queries:"
echo "  # List all files:"
echo "  sqlite3 $DB_FILE 'SELECT path FROM files LIMIT 10;'"
echo ""
echo "  # What does a file depend on:"
echo "  sqlite3 $DB_FILE \"SELECT f2.path FROM dependencies d JOIN files f1 ON d.target_id=f1.id JOIN files f2 ON d.dep_id=f2.id WHERE f1.path LIKE '%util.c' LIMIT 10;\""
echo ""
echo "  # What depends on a header (reverse lookup):"
echo "  sqlite3 $DB_FILE \"SELECT f1.path FROM dependencies d JOIN files f1 ON d.target_id=f1.id JOIN files f2 ON d.dep_id=f2.id WHERE f2.path LIKE '%util.h' LIMIT 10;\""
