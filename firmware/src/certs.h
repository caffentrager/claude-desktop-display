#pragma once

// Root CA bundle for api.anthropic.com. mbedTLS accepts concatenated PEM
// certs as a trust bundle. Multiple roots so this survives a server-side
// CA rotation without needing a firmware update:
//   GlobalSign Root CA      - current api.anthropic.com anchor (expires 2028-01-28)
//   ISRG Root X1            - Let's Encrypt (expires 2035-06-04)
//   DigiCert Global Root G2 - common rotation target (expires 2038-01-15)
//
// Adapted from https://github.com/oauramos/claude-usage-stick (MIT).
extern const char CA_BUNDLE[];
