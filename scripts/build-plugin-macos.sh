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
  "17.0"
)

VERSIONS=()
CONFIGURATION="Release"
BUILD_ROOT=""
ARTIFACTS_ROOT=""
GENERATOR=""
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

normalize_versions() {
  local -a raw_versions=("$@")
  local -A seen=()
  local -a normalized=()
  local entry part value

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
      if [[ -z "${seen[$value]+x}" ]]; then
        normalized+=("$value")
        seen[$value]=1
      fi
    done
  done

  printf '%s\n' "${normalized[@]}"
}

get_search_roots() {
  local -A seen=()
  local -a roots=()
  local root

  if [[ -n "${NUKE_INSTALL_ROOTS:-}" ]]; then
    IFS=':' read -r -a env_roots <<< "$NUKE_INSTALL_ROOTS"
    for root in "${env_roots[@]}"; do
      [[ -n "$root" ]] || continue
      if [[ -z "${seen[$root]+x}" ]]; then
        roots+=("$root")
        seen[$root]=1
      fi
    done
  fi

  for root in "/Applications" "/Applications/Foundry" "/usr/local" "/opt" "$HOME/Applications" "$HOME/apps"; do
    if [[ -z "${seen[$root]+x}" ]]; then
      roots+=("$root")
      seen[$root]=1
    fi
  done

  printf '%s\n' "${roots[@]}"
}

parse_nuke_install_name() {
  local name="$1"
  if [[ "$name" =~ ^Nuke([0-9]+\.[0-9]+)v([0-9]+)(\.app)?$ ]]; then
    printf '%s|%s|Nuke%sv%s\n' "${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}" "${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}"
  fi
}

get_installed_nukes() {
  local -A found_by_dir=()
  local -A best_patch=()
  local -A best_record=()
  local root dir name parsed minor patch full source install_dir

  while IFS= read -r root; do
    [[ -d "$root" ]] || continue

    name="$(basename "$root")"
    parsed="$(parse_nuke_install_name "$name" || true)"
    if [[ -n "$parsed" ]]; then
      install_dir="$root"
      if [[ "$name" == *.app && -d "${root}/Contents/MacOS" ]]; then
        install_dir="${root}/Contents/MacOS"
      fi
      found_by_dir["$install_dir"]="${parsed}|${install_dir}|filesystem"
    fi

    while IFS= read -r -d '' dir; do
      name="$(basename "$dir")"
      parsed="$(parse_nuke_install_name "$name" || true)"
      if [[ -n "$parsed" ]]; then
        install_dir="$dir"
        if [[ "$name" == *.app && -d "${dir}/Contents/MacOS" ]]; then
          install_dir="${dir}/Contents/MacOS"
        fi
        found_by_dir["$install_dir"]="${parsed}|${install_dir}|filesystem"
      fi
    done < <(find "$root" -mindepth 1 -maxdepth 1 -type d -name 'Nuke*' -print0 2>/dev/null)
  done < <(get_search_roots)

  for dir in "${!found_by_dir[@]}"; do
    IFS='|' read -r minor patch full _ source <<< "${found_by_dir[$dir]}"
    if [[ -z "${best_patch[$minor]+x}" || "$patch" -gt "${best_patch[$minor]}" ]]; then
      best_patch[$minor]="$patch"
      best_record[$minor]="${minor}|${full}|${dir}|${source}"
    fi
  done

  for minor in "${!best_record[@]}"; do
    printf '%s\n' "${best_record[$minor]}"
  done | sort -t'|' -k1,1V
}

show_targets() {
  local record minor full install_dir source
  while IFS='|' read -r minor full install_dir source; do
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
mapfile -t REQUESTED_VERSIONS < <(normalize_versions "${VERSIONS[@]}")

write_section "Scanning for Nuke installs"
mapfile -t INSTALLED_NUKES < <(get_installed_nukes)

if [[ ${#INSTALLED_NUKES[@]} -eq 0 ]]; then
  printf 'No installed Nuke versions were found. Set NUKE_INSTALL_ROOTS if your installs are in a custom location.\n' >&2
  exit 1
fi

declare -A INSTALLED_BY_MINOR=()
for record in "${INSTALLED_NUKES[@]}"; do
  IFS='|' read -r minor _ <<< "$record"
  INSTALLED_BY_MINOR["$minor"]="$record"
done

TARGETS=()
for version in "${REQUESTED_VERSIONS[@]}"; do
  if [[ -n "${INSTALLED_BY_MINOR[$version]+x}" ]]; then
    TARGETS+=("${INSTALLED_BY_MINOR[$version]}")
  fi
done

if [[ ${#TARGETS[@]} -eq 0 ]]; then
  requested_joined="$(IFS=', '; echo "${REQUESTED_VERSIONS[*]}")"
  available_joined="$(printf '%s\n' "${INSTALLED_NUKES[@]}" | cut -d'|' -f1 | tr '\n' ',' | sed 's/,$//' | sed 's/,/, /g')"
  printf 'None of the requested Nuke versions were found. Requested: %s. Available: %s\n' "$requested_joined" "$available_joined" >&2
  exit 1
fi

printf 'MinorVersion\tFullVersion\tInstallDir\tSource\n'
printf '%s\n' "${TARGETS[@]}" | show_targets

if [[ "$LIST_ONLY" -eq 1 ]]; then
  exit 0
fi

mkdir -p "$RESOLVED_BUILD_ROOT" "$RESOLVED_ARTIFACTS_ROOT"

for record in "${TARGETS[@]}"; do
  IFS='|' read -r minor_version full_version install_dir _ <<< "$record"
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
  )

  if [[ -n "$GENERATOR" ]]; then
    configure_args+=(-G "$GENERATOR")
  fi

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
