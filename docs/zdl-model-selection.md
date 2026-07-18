# ZDL Contextual Model Selection

## Decision

Use `de-zdl-lg` 4.0.0 (static embeddings) for the production host compiler.
Do not install or load `de-zdl-dist` in the normal compiler environment.

## Evidence

Both models were evaluated in isolated CPython 3.12 environments against the
same 16-case C09 corpus, with DWDSmor 0.18.0 Open Edition and analysis policy 1.
The checked-in raw result is
`test/data/contextual/zdl-model-benchmark.json`.

Host: Linux x86-64, 16 logical CPUs. Five corpus passes were run per process.
Peak RSS is whole-process high-water memory, so it includes spaCy, the model and
DWDSmor.

| Metric | Static `de-zdl-lg` | Transformer `de-zdl-dist` |
| --- | ---: | ---: |
| Context correct | 13/16 | 13/16 |
| Fused primary correct | 10/16 | 10/16 |
| Model load | 2.001 s | 2.740 s |
| Cold corpus | 0.121 s | 0.139 s |
| Warm corpus mean | 0.116 s | 0.130 s |
| Peak RSS | 1,267,703,808 B | 2,234,990,592 B |
| Model wheel | 627,548,130 B | 508,569,684 B |

The transformer produced no accuracy improvement on any gold case. It used
approximately 967 MB more peak host RAM, loaded about 37% slower, and processed
the warm corpus about 12% slower on CPU. Although its model wheel is smaller,
its PyTorch/transformer runtime also pulled a multi-gigabyte platform dependency
set in this Linux environment.

These are host costs only; neither model is uploaded to the reader. Static is
selected because equal observed accuracy makes the larger transient host memory
and dependency surface of the transformer unjustified.

## Reproduction

Install the ordinary static environment and a separate transformer benchmark
environment, then run:

```sh
.cache/contextual/.venv/bin/python \
  scripts/dictionary/contextual/benchmark_zdl_models.py \
  --repeats 5 \
  --python .cache/contextual/.venv-static/bin/python \
  --python .cache/contextual/.venv/bin/python \
  --output test/data/contextual/zdl-model-benchmark.json
```

`requirements-zdl-dist.lock` is a Linux x86-64 benchmark overlay, not a
production compiler dependency. Revisit this choice only if a broader corpus
shows a material transformer accuracy improvement.
