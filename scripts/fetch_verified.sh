#!/usr/bin/env bash
#
# Download an artifact over HTTPS and check it against a pinned SHA-256.
#
#   scripts/fetch_verified.sh <url> <sha256> <dest>
#   scripts/fetch_verified.sh --no-verify <url> <dest>
#
# Every artifact CI pulls off the network goes through here, so the transport
# hardening and the integrity check live in one place and cannot drift apart
# between call sites. --no-verify is for artifacts whose authenticity is
# established some other way, such as the LLVM archive key that the caller
# checks by fingerprint.
set -euo pipefail

verify=true
if [[ ${1-} == "--no-verify" ]]; then
    verify=false
    shift
fi

if { [[ $verify == true ]] && [[ $# -ne 3 ]]; } || { [[ $verify == false ]] && [[ $# -ne 2 ]]; }; then
    echo "usage: $0 <url> <sha256> <dest>" >&2
    echo "       $0 --no-verify <url> <dest>" >&2
    exit 2
fi

url=$1
if [[ $verify == true ]]; then
    sha256=$2
    dest=$3
else
    dest=$2
fi

# HTTPS only, and no downgrade to plaintext on redirect. TLS 1.2 is a floor
# rather than a cap, so 1.3 is still negotiated where the host offers it.
# --fail keeps an HTTP error page from landing in <dest> and being mistaken
# for content.
curl --proto '=https' --proto-redir '=https' --tlsv1.2 \
    --fail --silent --show-error --location \
    --retry 3 --retry-all-errors \
    --output "$dest" \
    "$url"

if [[ $verify == true ]]; then
    echo "$sha256  $dest" | sha256sum --check --strict
fi
