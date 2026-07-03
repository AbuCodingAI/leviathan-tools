# DevVM Notes

## Execution Model

`.dev` files are **data files, not executables**.

### No chmod +x needed
```bash
# Traditional binaries require executable permission
chmod +x myapp
./myapp

# .dev files work directly via VM interpreter
dev run myapp.dev          # Works even without execute permission
dev run ./path/myapp.dev   # Works, no chmod needed
```

### Why
- `.dev` is IR (intermediate representation), not machine code
- VM is the interpreter/executor
- Filesystem permission model doesn't apply

### Dev Run Behavior
```
dev run myapp.dev
  ↓
1. Read myapp.dev (data file)
2. Verify magic bytes (100, 101, 118 = "dev")
3. Check trust key (sandboxed or full execution)
4. Load IR instructions
5. Execute via VM
```

### Security Model
- **File permissions**: Irrelevant (VM controls access)
- **Trust key**: Embedded in .dev file (sandboxed if missing)
- **Syscalls**: Filtered by trust level

Users cannot accidentally make a `.dev` file executable at filesystem level. They can only control trust via signing.
