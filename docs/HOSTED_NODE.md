# Static hosted Nomad pages

`reticulum/hosted_node.h` provides an opt-in, application-independent POSIX
static page service on a `nomadnetwork.node` destination. It does not start a
daemon, publish private directories automatically, or announce at startup.

Create a runtime and private identity, then create the service with a dedicated
page directory and an explicit access policy. Publish each desired relative
path with `rns_hosted_node_publish_page(node, "index.mu")`; this registers the
upstream request path `/page/index.mu`. Nested paths such as `guide/start.mu`
are supported. The current runtime permits 16 handlers per destination.

Call `rns_hosted_node_announce` explicitly with the display name when ready.
After each `rns_runtime_poll`, call `rns_hosted_node_poll` to reap closed links.
Destroy the service before destroying its runtime. The service owns its inbound
links and must be used from the runtime owner thread, outside callbacks.

The service returns canonical binary MessagePack page responses through the
runtime's packet or Resource response machinery. Content is read afresh for
each request, capped at 16 KiB by default. The configurable upper bound is
8 MiB minus five encoding bytes, but actual transfer limits remain those of the
runtime's current single-segment Resource implementation. Setting a larger
storage bound does not add multi-segment transfer support.

## Safety and current boundaries

- Roots are pinned by directory descriptors. Each descendant is opened with
  `openat` and `O_NOFOLLOW`; symlinks and non-regular files are rejected.
- Hidden names, `..`, repeated separators, percent escapes, control characters,
  backslashes, anchors and query strings are rejected. Published paths currently
  use a conservative printable ASCII namespace.
- No executable page is executed or served as source. A regular
  `<page>.allowed` sidecar restricts that page to newline-delimited 16-byte
  identity hashes encoded as exactly 32 hexadecimal characters. The remote must
  identify on the link. Empty policies deny everyone; malformed, oversized,
  symlinked and executable policies fail closed. Policies are reread per request.
- Global allow-all, deny-all, identified-only and explicit identity allowlists
  are applied by the authenticated runtime request handler before file access.
- Dedicated roots and their trusted ancestors must be operator-controlled.
  Do not allow untrusted local users to mutate served files or ACL sidecars.
- The optional file root supports bounded **local** reads only. NomadNet 1.2.0
  file downloads use file-backed response Resources with filename metadata,
  which the runtime does not yet expose. No remote `/file/` handler is registered.
- Automatic scans, default index generation, executable pages,
  UTF-8 path names, refresh jobs, request statistics, daemon/TUI controls and
  streaming downloads remain unimplemented.

## Evidence

The implementation was behaviorally inspected against pinned
[NomadNet Node.py](https://github.com/markqvist/NomadNet/blob/475c0ee2a0388cf8470e7f1e90d5decb67b579ea/nomadnet/Node.py).
`test_hosted_node` covers bounds and filesystem rejection. The loopback
`test_hosted_node_link` covers global identification/allowlists, live sidecar
allow/deny/malformed changes, the upstream denial page, fresh small-page packet
responses, 2 KiB Resource responses and service lifetime. These are C-to-C tests,
not stock NomadNet interoperability evidence. Executable `.allowed` generators
from upstream remain unsupported rather than being run implicitly.
