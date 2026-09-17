# Compatibility Semantics

These behaviors come from the executable Python oracle even when they differ
from common browser or library expectations:

- A normal HTML end tag pops one stack level; attributes do not decode HTML
  entities; `<br/>` is parsed as a `br/` element.
- CSS comments are not fully supported and can affect the following rule.
- Later declarations in one block override earlier declarations even when the
  earlier declaration is `!important`.
- General `<img>` elements have no layout yet; emoji handling is limited to a
  single-character local PNG path.
- `--rtl` performs simplified right alignment rather than full bidirectional
  layout.

Preserve these semantics until a deliberate difference is measured and recorded
in `PORTING_PLAN.md`; standard-library defaults must not silently replace them.
Memory corruption is never a compatibility behavior; native invalid-input paths
return explicit errors.
