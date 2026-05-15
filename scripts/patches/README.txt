# QSOE patches against vendored upstream sources.
#
# Each .patch file in this directory is a unified diff that
# extract-sel4-riscv.sh applies after copying upstream sources
# into core/.  Patches are applied in lexicographic order.
#
# Naming: kernel-<area>-<one-line-purpose>.patch
#         musl-<area>-<purpose>.patch
#
# If 'patch -p1' fails to apply, the extract script aborts loudly —
# that's the signal that upstream changed and the patch needs a
# refresh.  Rebase against the new upstream source under
# sel4test-full/ and re-emit the diff.

