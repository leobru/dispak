#!/bin/sh
set -eu

usage()
{
    cat >&2 <<EOF
Usage: $0 [--arfa-dir DIR] [--besmtool PATH] [--force]

Create an empty ARFA archive containing:
  ARFA.ГП  owner=999999  length=1  from disk 2248 zone 53
  ДИМИП    owner=999999  length=2  from disk 2248 zones 55-56

Without --force, DIR must not contain an existing archive.
EOF
    exit 1
}

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
arfa_dir=${HOME:-/tmp}/.besm6/arfa
besmtool=
force=0

while [ "$#" -gt 0 ]; do
    case "$1" in
    --arfa-dir)
        [ "$#" -gt 1 ] || usage
        arfa_dir=$2
        shift 2
        ;;
    --arfa-dir=*)
        arfa_dir=${1#*=}
        shift
        ;;
    --besmtool)
        [ "$#" -gt 1 ] || usage
        besmtool=$2
        shift 2
        ;;
    --besmtool=*)
        besmtool=${1#*=}
        shift
        ;;
    --force)
        force=1
        shift
        ;;
    -h|--help)
        usage
        ;;
    *)
        usage
        ;;
    esac
done

if [ -z "$besmtool" ]; then
    if [ -x "$repo_dir/build/besmtool/besmtool" ]; then
        besmtool=$repo_dir/build/besmtool/besmtool
    else
        besmtool=besmtool
    fi
fi

if [ "$force" -eq 1 ]; then
    rm -rf -- "$arfa_dir"
elif [ -e "$arfa_dir/.catalog" ] || [ "$(find "$arfa_dir" -mindepth 1 -maxdepth 1 2>/dev/null | wc -l)" -ne 0 ]; then
    echo "$0: $arfa_dir is not empty; use --force to recreate it" >&2
    exit 1
fi

mkdir -p -- "$arfa_dir"

"$repo_dir/mkarfa.py" --arfa-dir "$arfa_dir" --owner 999999 --catalog ARFA
"$repo_dir/mkarfa.py" --arfa-dir "$arfa_dir" --owner 999999 --length 1 'ARFA.ГП'
"$repo_dir/mkarfa.py" --arfa-dir "$arfa_dir" --owner 999999 --length 2 'ДИМИП'

"$besmtool" --arfa-dir "$arfa_dir" --from-disk=2248 --from-start=053 --length=1 write 'ARFA.ГП'
"$besmtool" --arfa-dir "$arfa_dir" --from-disk=2248 --from-start=055 --length=2 write 'ДИМИП'

echo "Created ARFA archive in $arfa_dir"
