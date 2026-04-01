#!/bin/bash
#
# extract-sel4-riscv.sh
#
# Extracts seL4 kernel and elfloader sources for RISC-V 64-bit (qemu-riscv-virt)
# into a new simplified directory layout.
#
# If sel4test-full is not present in current directory, fetches it using repo.
#
# Usage:
#   ./extract-sel4-riscv.sh [output_dir]
#
# If output_dir is not specified, defaults to "./qsoe".
#
# Placement file format:
#   # Comment lines start with #
#   source/path/file.c                    dest/path/file.c       # Single file
#   source/dir/                           dest/dir/              # Entire directory (recursive)
#   source/dir/*.c                        dest/dir/              # Glob pattern
#   source/dir/**/*.h                     dest/dir/              # Recursive glob
#
# Exclusion patterns (processed after all copies):
#   !exclude/pattern/                     # Exclude entire directory
#   !exclude/**/*.txt                     # Exclude by glob pattern
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLACEMENT_FILE="${SCRIPT_DIR}/placement.txt"
MANIFEST_URL="https://github.com/seL4/sel4test-manifest.git"
SOURCE_DIR_NAME="sel4test-full"
OUTPUT_DIR="${1:-./qsoe}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

info()  { echo -e "${GREEN}[INFO]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*" >&2; }
step()  { echo -e "${CYAN}[STEP]${NC} $*"; }

# Check placement file exists
if [[ ! -f "${PLACEMENT_FILE}" ]]; then
    error "Placement file not found: ${PLACEMENT_FILE}"
    error "Please ensure placement.txt is in the same directory as this script."
    exit 1
fi

# Determine source directory
SOURCE_DIR=""

# Option 1: Check if we're inside sel4test-full (kernel/ exists relative to script)
if [[ -d "${SCRIPT_DIR}/kernel" ]] && [[ -d "${SCRIPT_DIR}/tools/seL4/elfloader-tool" ]]; then
    info "Running from within sel4test-full checkout."
    SOURCE_DIR="${SCRIPT_DIR}"
fi

# Option 2: Check if sel4test-full exists in current working directory
if [[ -z "${SOURCE_DIR}" ]] && [[ -d "./${SOURCE_DIR_NAME}/kernel" ]]; then
    info "Found ${SOURCE_DIR_NAME} in current directory."
    SOURCE_DIR="$(pwd)/${SOURCE_DIR_NAME}"
fi

# Option 3: Need to fetch using repo
if [[ -z "${SOURCE_DIR}" ]]; then
    step "sel4test-full not found. Fetching using repo..."

    # Check if repo tool is available
    if ! command -v repo &> /dev/null; then
        error "'repo' tool not found."
        echo
        echo "Install it with:"
        echo "  mkdir -p ~/.local/bin"
        echo "  curl https://storage.googleapis.com/git-repo-downloads/repo > ~/.local/bin/repo"
        echo "  chmod a+x ~/.local/bin/repo"
        echo "  export PATH=~/.local/bin:\$PATH"
        echo
        exit 1
    fi

    # Create and enter source directory
    mkdir -p "${SOURCE_DIR_NAME}"
    cd "${SOURCE_DIR_NAME}"

    info "Initializing repo with manifest: ${MANIFEST_URL}"
    repo init -u "${MANIFEST_URL}"

    info "Syncing repositories (this may take a while)..."
    repo sync -j4

    cd ..
    SOURCE_DIR="$(pwd)/${SOURCE_DIR_NAME}"
    info "Fetch complete: ${SOURCE_DIR}"
fi

# Verify source directory has expected structure
if [[ ! -d "${SOURCE_DIR}/kernel" ]]; then
    error "Source directory missing kernel/: ${SOURCE_DIR}"
    exit 1
fi

if [[ ! -d "${SOURCE_DIR}/tools/seL4/elfloader-tool" ]]; then
    error "Source directory missing tools/seL4/elfloader-tool/: ${SOURCE_DIR}"
    exit 1
fi

info "Source directory: ${SOURCE_DIR}"

# Create output directory
OUTPUT_DIR="$(realpath -m "${OUTPUT_DIR}")"

if [[ -d "${OUTPUT_DIR}" ]]; then
    warn "Output directory already exists: ${OUTPUT_DIR}"
    read -p "Remove and recreate? [y/N] " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        rm -rf "${OUTPUT_DIR}"
    else
        error "Aborting."
        exit 1
    fi
fi

mkdir -p "${OUTPUT_DIR}"
info "Output directory: ${OUTPUT_DIR}"

# Counters
copied=0
skipped=0
missing=0
patterns=0
excluded=0

# Collect exclusion patterns to process after copies
declare -a EXCLUSIONS

# Function to copy a single file
copy_file() {
    local src_path="$1"
    local dst_path="$2"

    if [[ ! -f "${src_path}" ]]; then
        return 1
    fi

    local dst_dir
    dst_dir=$(dirname "${dst_path}")
    mkdir -p "${dst_dir}"
    cp "${src_path}" "${dst_path}"
    return 0
}

# Function to process a glob/directory pattern
process_pattern() {
    local src_pattern="$1"
    local dst_base="$2"
    local src_base
    local count=0

    # Remove trailing slash from destination
    dst_base="${dst_base%/}"

    # Determine the base directory (part before any wildcard or trailing slash)
    if [[ "${src_pattern}" == */ ]]; then
        # Directory copy: source/dir/ -> copy all files recursively
        src_base="${src_pattern%/}"
        if [[ ! -d "${SOURCE_DIR}/${src_base}" ]]; then
            warn "Source directory not found: ${src_base}"
            return 0
        fi

        while IFS= read -r -d '' file; do
            local rel_path="${file#${SOURCE_DIR}/${src_base}/}"
            local dst_path="${OUTPUT_DIR}/${dst_base}/${rel_path}"
            if copy_file "${file}" "${dst_path}"; then
                ((++count))
            fi
        done < <(find "${SOURCE_DIR}/${src_base}" -type f -print0)

    elif [[ "${src_pattern}" == *"**"* ]]; then
        # Recursive glob: source/dir/**/*.c
        src_base="${src_pattern%%\**}"
        src_base="${src_base%/}"
        local glob_part="${src_pattern#${src_base}/}"

        if [[ ! -d "${SOURCE_DIR}/${src_base}" ]]; then
            warn "Source directory not found: ${src_base}"
            return 0
        fi

        # Convert glob to find pattern
        local name_pattern="${glob_part##*/}"

        while IFS= read -r -d '' file; do
            local rel_path="${file#${SOURCE_DIR}/${src_base}/}"
            local dst_path="${OUTPUT_DIR}/${dst_base}/${rel_path}"
            if copy_file "${file}" "${dst_path}"; then
                ((++count))
            fi
        done < <(find "${SOURCE_DIR}/${src_base}" -type f -name "${name_pattern}" -print0)

    elif [[ "${src_pattern}" == *"*"* ]]; then
        # Simple glob: source/dir/*.c
        src_base=$(dirname "${src_pattern}")
        local glob_part=$(basename "${src_pattern}")

        if [[ ! -d "${SOURCE_DIR}/${src_base}" ]]; then
            warn "Source directory not found: ${src_base}"
            return 0
        fi

        while IFS= read -r -d '' file; do
            local filename=$(basename "${file}")
            local dst_path="${OUTPUT_DIR}/${dst_base}/${filename}"
            if copy_file "${file}" "${dst_path}"; then
                ((++count))
            fi
        done < <(find "${SOURCE_DIR}/${src_base}" -maxdepth 1 -type f -name "${glob_part}" -print0)
    fi

    echo "${count}"
}

# Process placement file
step "Processing placement.txt..."

while IFS= read -r line; do
    # Skip empty lines and comments
    [[ -z "${line}" ]] && continue
    [[ "${line}" =~ ^[[:space:]]*# ]] && continue

    # Parse source and destination (whitespace separated)
    src=$(echo "${line}" | awk '{print $1}')
    dst=$(echo "${line}" | awk '{print $2}')

    # Check if this is an exclusion pattern
    if [[ "${src}" == "!"* ]]; then
        # Store exclusion pattern for later processing
        EXCLUSIONS+=("${src#!}")
        continue
    fi

    # Skip if we couldn't parse both fields
    if [[ -z "${src}" ]] || [[ -z "${dst}" ]]; then
        warn "Skipping malformed line: ${line}"
        ((++skipped))
        continue
    fi

    # Check if this is a pattern (contains * or ends with /)
    if [[ "${src}" == *"*"* ]] || [[ "${src}" == */ ]]; then
        # Pattern/directory copy
        count=$(process_pattern "${src}" "${dst}")
        if [[ "${count}" -gt 0 ]]; then
            ((++patterns))
            copied=$((copied + count))
        else
            warn "No files matched pattern: ${src}"
            ((++missing))
        fi
    else
        # Single file copy
        src_path="${SOURCE_DIR}/${src}"
        dst_path="${OUTPUT_DIR}/${dst}"

        if [[ ! -f "${src_path}" ]]; then
            warn "Source file not found: ${src}"
            ((++missing))
            continue
        fi

        if copy_file "${src_path}" "${dst_path}"; then
            ((++copied))
        fi
    fi

done < "${PLACEMENT_FILE}"

# Process exclusion patterns
if [[ ${#EXCLUSIONS[@]} -gt 0 ]]; then
    step "Processing ${#EXCLUSIONS[@]} exclusion patterns..."
    for excl in "${EXCLUSIONS[@]}"; do
        excl_path="${OUTPUT_DIR}/${excl}"

        if [[ "${excl}" == */ ]]; then
            # Directory exclusion
            if [[ -d "${excl_path%/}" ]]; then
                count=$(find "${excl_path%/}" -type f | wc -l)
                rm -rf "${excl_path%/}"
                excluded=$((excluded + count))
            fi
        elif [[ "${excl}" == *"**"* ]]; then
            # Recursive glob exclusion
            base="${excl%%\**}"
            base="${base%/}"
            pattern="${excl##*/}"
            if [[ -d "${OUTPUT_DIR}/${base}" ]]; then
                while IFS= read -r -d '' file; do
                    rm -f "${file}"
                    ((++excluded))
                done < <(find "${OUTPUT_DIR}/${base}" -type f -name "${pattern}" -print0 2>/dev/null)
            fi
        elif [[ "${excl}" == *"*"* ]]; then
            # Simple glob exclusion
            base=$(dirname "${excl}")
            pattern=$(basename "${excl}")
            if [[ -d "${OUTPUT_DIR}/${base}" ]]; then
                while IFS= read -r -d '' file; do
                    rm -f "${file}"
                    ((++excluded))
                done < <(find "${OUTPUT_DIR}/${base}" -maxdepth 1 -type f -name "${pattern}" -print0 2>/dev/null)
            fi
        else
            # Single file/dir exclusion
            if [[ -e "${excl_path}" ]]; then
                if [[ -d "${excl_path}" ]]; then
                    count=$(find "${excl_path}" -type f | wc -l)
                    rm -rf "${excl_path}"
                    excluded=$((excluded + count))
                else
                    rm -f "${excl_path}"
                    ((++excluded))
                fi
            fi
        fi
    done

    # Clean up empty directories
    find "${OUTPUT_DIR}" -type d -empty -delete 2>/dev/null || true
fi

echo
info "Extraction complete!"
echo "  Copied:   ${copied} files"
echo "  Excluded: ${excluded} files"
echo "  Patterns: ${patterns} processed"
echo "  Missing:  ${missing} files/patterns"
echo "  Skipped:  ${skipped} lines"
echo
info "Output: ${OUTPUT_DIR}"

# Show directory structure summary (limit depth for large trees)
echo
step "Directory structure (top levels):"
find "${OUTPUT_DIR}" -maxdepth 4 -type d | sort | while read -r dir; do
    depth=$(echo "${dir}" | sed "s|${OUTPUT_DIR}||" | tr -cd '/' | wc -c)
    indent=$(printf '%*s' $((depth * 2)) '')
    basename="${dir##*/}"
    if [[ "${dir}" == "${OUTPUT_DIR}" ]]; then
        echo "  ${OUTPUT_DIR##*/}/"
    else
        count=$(find "${dir}" -maxdepth 1 -type f | wc -l)
        subdir_count=$(find "${dir}" -maxdepth 1 -type d | wc -l)
        subdir_count=$((subdir_count - 1))  # exclude self
        if [[ ${subdir_count} -gt 0 ]]; then
            echo "  ${indent}${basename}/ (${count} files, ${subdir_count} subdirs)"
        else
            echo "  ${indent}${basename}/ (${count} files)"
        fi
    fi
done

# File type summary
echo
step "File summary:"
echo "  C sources (.c):        $(find "${OUTPUT_DIR}" -name '*.c' | wc -l)"
echo "  Assembly (.S/.s):      $(find "${OUTPUT_DIR}" \( -name '*.S' -o -name '*.s' \) | wc -l)"
echo "  Headers (.h):          $(find "${OUTPUT_DIR}" -name '*.h' | wc -l)"
echo "  Linker scripts (.lds): $(find "${OUTPUT_DIR}" -name '*.lds' | wc -l)"

echo
info "Done."
