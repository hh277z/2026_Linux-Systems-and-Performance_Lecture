# Implementation Guide
Reuse your Assignment 9 memhog and psimon. Implement memmon, oomparse, the memdiag extension and alloctrace yourself according to the assignment.
This directory supplies only lmk_policy.c as the small policy exercise. Baseline ranking is runnable; PSI/importance return ENOSYS until implemented. The helper library handles registration/identity/timing and never broadens the candidate set.
Read student_support/build_earlyoom.py: experimental mode is selected with --lab and requires a protected registry. Upstream options are preserved by invoking the binary without --lab, but NEVER run that unrestricted path against a shared machine.

