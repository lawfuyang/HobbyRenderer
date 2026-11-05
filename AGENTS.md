## 1. Think Before Coding

**Don't assume. Don't hide confusion. Surface tradeoffs.**

Before implementing:
- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them - don't pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.

## 2. Simplicity First

**Minimum code that solves the problem. Nothing speculative.**

- No features beyond what was asked.
- No abstractions for single-use code.
- No "flexibility" or "configurability" that wasn't requested.
- No error handling for impossible scenarios.
- If you write 200 lines and it could be 50, rewrite it.

Ask yourself: "Would a senior engineer say this is overcomplicated?" If yes, simplify.

## 3. Surgical Changes

**Touch only what you must. Clean up only your own mess.**

When editing existing code:
- Don't "improve" adjacent code, comments, or formatting.
- Don't refactor things that aren't broken.
- Match existing style, even if you'd do it differently.
- If you notice unrelated dead code, mention it - don't delete it.

When your changes create orphans:
- Remove imports/variables/functions that YOUR changes made unused.
- Don't remove pre-existing dead code unless asked.

The test: Every changed line should trace directly to the user's request.

## 4. Goal-Driven Execution

**Define success criteria. Loop until verified.**

Transform tasks into verifiable goals:
- "Add validation" → "Write tests for invalid inputs, then make them pass"
- "Fix the bug" → "Write a test that reproduces it, then make it pass"
- "Refactor X" → "Ensure tests pass before and after"

For multi-step tasks, state a brief plan:
```
1. [Step] → verify: [check]
2. [Step] → verify: [check]
3. [Step] → verify: [check]
```

Strong success criteria let you loop independently. Weak criteria ("make it work") require constant clarification.

---

**These guidelines are working if:** fewer unnecessary changes in diffs, fewer rewrites due to overcomplication, and clarifying questions come before implementation rather than after mistakes.

# Build Pipeline

Detailed documentation on the renderer architecture, IRenderer, Render Graph,
Resource Binding, nvrhi, srrhi, ShaderIDs, and the full build pipeline lives in
**[PIPELINE.md](PIPELINE.md)** — refer to that file for comprehensive pipeline and
codebase usage documentation.

# Debugging with captures

To take, read, process or debug a PIX `.wpix` capture, follow
**[docs/pix-capture-debugging.md](docs/pix-capture-debugging.md)** — capture switches,
every `pixtool.exe` command with examples, recipes for event lists / hardware counters /
per-pass images, the known tool bugs on this machine, and what needs the PIX GUI instead.
RenderDoc captures are the other half: `--renderdoc*` switches, see the same document's
introduction.

### Quick Reference

| Step | Command | Description |
|---|---|---|
| SRRHI generation | `build_srrhi` | `.sr` files → C++ headers + HLSL includes |
| Shader IDs | `build_shaderids` | `.cfg` files → `ShaderIDs.h` |
| Shader compilation | `build_shaders` | `.hlsl` files → DXIL binaries |
| C++ build | `${PROJECT_NAME}` | Compiles and links the main executable |

Build order is enforced by CMake dependencies: `build_srrhi → build_shaders → exe`
and `build_shaderids → exe`.

**Never manually edit** files under `src/shaders/srrhi/` or `src/shaders/ShaderIDs.h`.

---

# Naming

* **Types**: `UpperCamelCase` (nouns)
* **Functions**: `UpperCamelCase` (nouns)
* **bool**: `b*` (e.g. `bIsValid`)
* **Local vars**: `lowerCamelCase`
* **Members**: `m_UpperCamelCase`
* **Static members**: `ms_UpperCamelCase`
* **Constants**: `SCREAMING_SNAKE_CASE` or `kLowerCamelCase`
* **Globals**: `g_UpperCamelCase`
* **Static globals**: `gs_UpperCamelCase`

- Avoid `auto` (except for container iterators)
- Max line length: 140 columns
- Prefer `std::string_view` over `const std::string&`
- Use `const char*` for guaranteed hardcoded string inputs

### Type Aliases (from `pch.h`)

Use the following typedefs from `pch.h` instead of raw `DirectX::` namespace types:

| Typedef | DirectX Type |
|---|---|
| `Vector` | `DirectX::XMVECTOR` |
| `Matrix` | `DirectX::XMFLOAT4X4` |
| `Vector2` | `DirectX::XMFLOAT2` |
| `Vector3` | `DirectX::XMFLOAT3` |
| `Vector4` | `DirectX::XMFLOAT4` |
| `Quaternion` | `DirectX::XMFLOAT4` |
| `Vector3A` | `DirectX::XMFLOAT3A` |
| `Vector4A` | `DirectX::XMFLOAT4A` |
| `Vector2U` | `DirectX::XMUINT2` |
| `Vector2I` | `DirectX::XMINT2` |
| `Vector3U` | `DirectX::XMUINT3` |
| `Vector3I` | `DirectX::XMINT3` |
| `Sphere` | `DirectX::BoundingSphere` |
| `AABB` | `DirectX::BoundingBox` |
| `OBB` | `DirectX::BoundingOrientedBox` |
| `Frustum` | `DirectX::BoundingFrustum` |

**Always use the typedefs.** Never write `DirectX::XMFLOAT3`, `DirectX::XMVECTOR`, etc. directly in code.

---

Place all C/C++ standard library #includes in pch.h. Do not include standard headers directly in source or header files.

---

# Asserts

**A value that must be valid is asserted, not quietly tolerated.** Before adding a fallback, ask
whether the value is legitimately absent at that point of call:

* **Structural invariants — assert.** A pointer, handle or index the caller just resolved; a member
  construction guarantees; an argument whose contract says non-null; a table index derived from
  loaded data. A null or out-of-range value there is a BUG, not a state: `SDL_assert(ptr != nullptr);`
  and let the debugger stop on it. Do not write the defensive branch that hides it.
* **Legitimate optional state — keep the guard, and say why.** Assert only what is actually guaranteed, and comment the
  guard with the reason it is expected.
* **Argument validation on a resolved handle — assert the contract AND keep the caller safe.** never let a bad index or a null handle reach a dereference on that path.

Asserts are development scaffolding and must be TRUE, never aspirational: an assert that fires during
an ordinary run is worse than the fallback it replaced.