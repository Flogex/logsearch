# DuckDB BoundIndex — TODO

- k-way merge of InvertedIndexes during index build instead of pairwise merge.
  The benefit would be that we safe a few reallocations of vectors and a few move operations,
  and we could leave the last InvertedIndex non-sealed.
  The question would be how to store the InvertedIndex in the global state and if we should seal them already in Combine.
  If we don't seal in Combine, we keep many Memtables around.
