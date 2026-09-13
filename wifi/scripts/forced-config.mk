# --- forced out-of-tree config (PCIe bring-up build) ---
# Spliced after the brcmfmac Makefile header; pattern: kimptoc phase5 Makefile.
# Values mirror the nvkp kernel config, PLUS explicit -DDEBUG:
# the out-of-tree DEBUG flag from CONFIG_BRCMDBG does not propagate to debug.c,
# which makes debug.c collide with the stubs in debug.h (redefinitions).
CONFIG_BRCMFMAC := m
CONFIG_BRCMFMAC_SDIO := y
CONFIG_BRCMFMAC_USB := y
CONFIG_BRCMFMAC_PCIE := m
CONFIG_BRCMFMAC_PROTO_BCDC := m
CONFIG_BRCMFMAC_PROTO_MSGBUF := m
CONFIG_BRCMDBG := y
CONFIG_BRCM_TRACING := y
ccflags-y += -DDEBUG
