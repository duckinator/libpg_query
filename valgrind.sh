#!/bin/sh
make VALGRIND=1 test/summary_truncate && valgrind --leak-check=full --gen-suppressions=all --suppressions=test/valgrind.supp --time-stamp=yes --error-markers=VALGRINDERROR-BEGIN,VALGRINDERROR-END --trace-children=yes --show-leak-kinds=all --error-exitcode=1 --errors-for-leak-kinds=all --num-callers=50 test/summary_truncate
