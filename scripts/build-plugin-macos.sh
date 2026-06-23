#!/usr/bin/env bash

set -euo pipefail

DEFAULT_VERSIONS=(
  "13.2"
  "14.0"
  "14.1"
  "14.2"
  "15.0"
  "15.1"
  "15.2"
  "16.0"
  "16.1"
  "17.0"
)

VERSIONS=()
REQUESTED_VERSIONS=()
SEARCH_ROOTS=()
INSTALLED_NUKES=()
TARGETS=()

CONFIGURATION="Release"
BUILD_ROOT=""
ARTIFACTS_ROOT=""
GENERATOR=""
ARCHITECTURES="x86_64;arm64"
LIST_ONLY=0
CLEAN=0

write_section() {
  printf '\n== %s ==\n' "$1"
}

repo_root() {
  cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd
}

resolve_path_or_default() {
  local configured="$1"
  local fallback_relative="$2"

  if [[ -z "$configured" ]]; then
    printf '%s/%s\n' "$REPO_ROOT" "$fallback_relative"
    return
  fi

  if [[ -e "$configured" ]]; then
    (
      cd "$configured" 2>/dev/null && pwd
    ) || printf '%s\n' "$configured"
    return
  fi

  printf '%s\n' "$configured"
}

append_unique() {
  local array_name="$1"
  local value="$2"
  local existing
  eval "local current=(\"\${${array_name}[@]-}\")"

  for existing in "${current[@]}"; do
    if [[ "$existing" == "$value" ]]; then
      return
    fi
  done

  eval "${array_name}+=(\"\$value\")"
}

normalize_versions() {
  local entry part value
  local raw_versions=("$@")

  REQUESTED_VERSIONS=()
  if [[ ${#raw_versions[@]} -eq 0 ]]; then
    raw_versions=("${DEFAULT_VERSIONS[@]}")
  fi

  for entry in "${raw_versions[@]}"; do
    IFS=',' read -r -a parts <<< "$entry"
    for part in "${parts[@]}"; do
      value="${part//[[:space:]]/}"
      [[ -n "$value" ]] || continue
      if [[ "$value" =~ ^[0-9]+$ ]]; then
        value="${value}.0"
      fi
      append_unique REQUESTED_VERSIONS "$value"
    done
  done
}

get_search_roots() {
  local root

  SEARCH_ROOTS=()
  if [[ -n "${NUKE_INSTALL_ROOTS:-}" ]]; then
    IFS=':' read -r -a env_roots <<< "$NUKE_INSTALL_ROOTS"
    for root in "${env_roots[@]}"; do
      [[ -n "$root" ]] || continue
      append_unique SEARCH_ROOTS "$root"
    done
  fi

  for root in "/Applications" "/Applications/Foundry" "/usr/local" "/opt" "$HOME/Applications" "$HOME/apps"; do
    append_unique SEARCH_ROOTS "$root"
  done
}

parse_nuke_install_name() {
  local name="$1"
  if [[ "$name" =~ ^Nuke([0-9]+\.[0-9]+)v([0-9]+)(\.app)?$ ]]; then
    printf '%s|%s|Nuke%sv%s\n' "${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}" "${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}"
  fi
}

find_record_index_by_minor() {
  local array_name="$1"
  local target_minor="$2"
  local records index record minor
  eval "records=(\"\${${array_name}[@]-}\")"

  for ((index = 0; index < ${#records[@]}; ++index)); do
    record="${records[$index]}"
    IFS='|' read -r minor _rest <<< "$record"
    if [[ "$minor" == "$target_minor" ]]; then
      printf '%s\n' "$index"
      return
    fi
  done

  printf '%s\n' "-1"
}

find_record_by_minor() {
  local array_name="$1"
  local target_minor="$2"
  local records record minor
  eval "records=(\"\${${array_name}[@]-}\")"

  for record in "${records[@]}"; do
    IFS='|' read -r minor _rest <<< "$record"
    if [[ "$minor" == "$target_minor" ]]; then
      printf '%s\n' "$record"
      return 0
    fi
  done

  return 1
}

get_installed_nukes() {
  local candidate_records=()
  local best_records=()
  local root dir name parsed install_dir record index existing_patch minor patch

  get_search_roots

  for root in "${SEARCH_ROOTS[@]}"; do
    [[ -d "$root" ]] || continue

    name="$(basename "$root")"
    parsed="$(parse_nuke_install_name "$name" || true)"
    if [[ -n "$parsed" ]]; then
      install_dir="$root"
      if [[ "$name" == *.app && -d "${root}/Contents/MacOS" ]]; then
        install_dir="${root}/Contents/MacOS"
      fi
      append_unique candidate_records "${parsed}|${install_dir}|filesystem"
    fi

    while IFS= read -r -d '' dir; do
      name="$(basename "$dir")"
      parsed="$(parse_nuke_install_name "$name" || true)"
      if [[ -n "$parsed" ]]; then
        install_dir="$dir"
        if [[ "$name" == *.app && -d "${dir}/Contents/MacOS" ]]; then
          install_dir="${dir}/Contents/MacOS"
        fi
        append_unique candidate_records "${parsed}|${install_dir}|filesystem"
      fi
    done < <(find "$root" -mindepth 1 -maxdepth 1 -type d -name 'Nuke*' -print0 2>/dev/null)
  done

  for record in "${candidate_records[@]}"; do
    IFS='|' read -r minor patch _rest <<< "$record"
    index="$(find_record_index_by_minor best_records "$minor")"

    if [[ "$index" == "-1" ]]; then
      best_records+=("$record")
      continue
    fi

    IFS='|' read -r _ existing_patch _rest <<< "${best_records[$index]}"
    if [[ "$patch" -gt "$existing_patch" ]]; then
      best_records[$index]="$record"
    fi
  done

  INSTALLED_NUKES=()
  while IFS= read -r line; do
    [[ -n "$line" ]] || continue
    INSTALLED_NUKES+=("$line")
  done < <(printf '%s\n' "${best_records[@]}" | sort -t'|' -k1,1)
}

show_targets() {
  local record minor patch full install_dir source
  for record in "$@"; do
    IFS='|' read -r minor patch full install_dir source <<< "$record"
    printf '%s\t%s\t%s\t%s\n' "$minor" "$full" "$install_dir" "$source"
  done
}

print_command() {
  local part
  for part in "$@"; do
    printf '%q ' "$part"
  done
  printf '\n'
}

copy_build_artifact() {
  local build_dir="$1"
  local configuration="$2"
  local minor_version="$3"
  local artifacts_dir="$4"
  local artifact=""
  local candidate

  for candidate in \
    "${build_dir}/${configuration}/gifWriter.dylib" \
    "${build_dir}/gifWriter.dylib"; do
    if [[ -f "$candidate" ]]; then
      artifact="$candidate"
      break
    fi
  done

  if [[ -z "$artifact" ]]; then
    printf 'Warning: no built gifWriter artifact was found for %s.\n' "$minor_version" >&2
    return
  fi

  mkdir -p "${artifacts_dir}/${minor_version}"
  cp -f "$artifact" "${artifacts_dir}/${minor_version}/gifWriter.dylib"

  if command -v lipo >/dev/null 2>&1; then
    printf 'Artifact architectures: %s\n' "$(lipo -archs "${artifacts_dir}/${minor_version}/gifWriter.dylib")"
  fi
}

usage() {
  cat <<'EOF'
Usage: build-plugin-macos.sh [options]

Options:
  --versions <list>       Comma-separated or repeated minor versions, for example 15.1,16.0
  --configuration <name>  Release, Debug, RelWithDebInfo, or MinSizeRel
  --build-root <path>     Override the build directory root
  --artifacts-root <path> Override the artifacts directory root
  --generator <name>      Override the CMake generator
  --architectures <list>  macOS architectures, default: x86_64;arm64
  --list-only             Show detected targets without building
  --clean                 Remove the per-version build directory before configuring
  --help                  Show this help text
EOF
}

require_value() {
  local option_name="$1"
  if [[ $# -lt 2 || -z "${2:-}" ]]; then
    printf 'Missing value for %s\n\n' "$option_name" >&2
    usage
    exit 1
  fi
}

normalize_architectures() {
  local raw_value="$1"
  local cleaned="${raw_value// /}"
  cleaned="${cleaned//,/;}"
  printf '%s\n' "$cleaned"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --versions)
      shift
      [[ $# -gt 0 ]] || { usage; exit 1; }
      while [[ $# -gt 0 && "$1" != --* ]]; do
        VERSIONS+=("$1")
        shift
      done
      continue
      ;;
    --configuration)
      shift
      require_value --configuration "${1:-}"
      CONFIGURATION="${1:-}"
      ;;
    --build-root)
      shift
      require_value --build-root "${1:-}"
      BUILD_ROOT="${1:-}"
      ;;
    --artifacts-root)
      shift
      require_value --artifacts-root "${1:-}"
      ARTIFACTS_ROOT="${1:-}"
      ;;
    --generator)
      shift
      require_value --generator "${1:-}"
      GENERATOR="${1:-}"
      ;;
    --architectures)
      shift
      require_value --architectures "${1:-}"
      ARCHITECTURES="$(normalize_architectures "${1:-}")"
      ;;
    --list-only)
      LIST_ONLY=1
      ;;
    --clean)
      CLEAN=1
      ;;
    --help)
      usage
      exit 0
      ;;
    *)
      printf 'Unknown argument: %s\n\n' "$1" >&2
      usage
      exit 1
      ;;
  esac

  shift
done

case "$CONFIGURATION" in
  Release|Debug|RelWithDebInfo|MinSizeRel)
    ;;
  *)
    printf 'Unsupported configuration: %s\n' "$CONFIGURATION" >&2
    exit 1
    ;;
esac

REPO_ROOT="$(repo_root)"
RESOLVED_BUILD_ROOT="$(resolve_path_or_default "$BUILD_ROOT" "build")"
RESOLVED_ARTIFACTS_ROOT="$(resolve_path_or_default "$ARTIFACTS_ROOT" "artifacts")"

if [[ ${#VERSIONS[@]} -eq 0 ]]; then
  normalize_versions
else
  normalize_versions "${VERSIONS[@]}"
fi

write_section "Scanning for Nuke installs"
get_installed_nukes

if [[ ${#INSTALLED_NUKES[@]} -eq 0 ]]; then
  printf 'No installed Nuke versions were found. Set NUKE_INSTALL_ROOTS if your installs are in a custom location.\n' >&2
  exit 1
fi

TARGETS=()
for version in "${REQUESTED_VERSIONS[@]}"; do
  record="$(find_record_by_minor INSTALLED_NUKES "$version" || true)"
  if [[ -n "$record" ]]; then
    TARGETS+=("$record")
  fi
done

if [[ ${#TARGETS[@]} -eq 0 ]]; then
  requested_joined="$(IFS=', '; echo "${REQUESTED_VERSIONS[*]}")"
  available_joined="$(printf '%s\n' "${INSTALLED_NUKES[@]}" | cut -d'|' -f1 | tr '\n' ',' | sed 's/,$//' | sed 's/,/, /g')"
  printf 'None of the requested Nuke versions were found. Requested: %s. Available: %s\n' "$requested_joined" "$available_joined" >&2
  exit 1
fi

printf 'MinorVersion\tFullVersion\tInstallDir\tSource\n'
show_targets "${TARGETS[@]}"

if [[ "$LIST_ONLY" -eq 1 ]]; then
  exit 0
fi

mkdir -p "$RESOLVED_BUILD_ROOT" "$RESOLVED_ARTIFACTS_ROOT"

for record in "${TARGETS[@]}"; do
  IFS='|' read -r minor_version _patch full_version install_dir _source <<< "$record"
  build_dir="${RESOLVED_BUILD_ROOT}/${full_version}"

  write_section "Building against ${full_version}"

  if [[ "$CLEAN" -eq 1 && -d "$build_dir" ]]; then
    rm -rf "$build_dir"
  fi

  configure_args=(
    cmake
    -S "$REPO_ROOT"
    -B "$build_dir"
    "-DNUKE_ROOT=${install_dir}"
    "-DCMAKE_PREFIX_PATH=${install_dir}"
    "-DCMAKE_BUILD_TYPE=${CONFIGURATION}"
    "-DCMAKE_OSX_ARCHITECTURES=${ARCHITECTURES}"
  )

  if [[ -n "$GENERATOR" ]]; then
    configure_args+=(-G "$GENERATOR")
  fi

  printf 'Using architectures: %s\n' "$ARCHITECTURES"
  print_command "${configure_args[@]}"
  "${configure_args[@]}"

  build_args=(
    cmake
    --build "$build_dir"
    --config "$CONFIGURATION"
  )

  print_command "${build_args[@]}"
  "${build_args[@]}"

  copy_build_artifact "$build_dir" "$CONFIGURATION" "$minor_version" "$RESOLVED_ARTIFACTS_ROOT"
done

write_section "Done"
printf 'Artifacts: %s\n' "$RESOLVED_ARTIFACTS_ROOT"
