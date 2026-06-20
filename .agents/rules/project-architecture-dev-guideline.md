---
trigger: model_decision
description: "Project architecture and module structure guidelines: use external READMEs for usage and internal DESIGN.md for implementation details. Prioritize reading documentation before development, and keep documents updated."
---

# GreatHole Module Documentation & Architecture Rules

To maintain high development velocity and clear abstraction boundaries, developers and agents must interact with each module and submodule using a documentation-first approach. 

Documentation in this project is split into two categories based on target audience and purpose:

1. **`README.md` (External Reference)**
   - **Target**: Consumers of the module.
   - **Focus**: External usage, public APIs, integration guidelines, and configuration options.
   - **Goal**: Help users utilize the module's public interfaces without needing to understand its internal structure.

2. **`DESIGN.md` (Internal Knowledge)**
   - **Target**: Developers or agents modifying the module itself.
   - **Focus**: Internal architecture, design decisions, data structures, state machines, protocol specs, and implementation details.
   - **Goal**: Help developers quickly understand the inner workings of the module and catch up on design choices.

---

## Guidelines for Submodule Exploration & Updates

- **Read Documentation Before Development**:
  - Prioritize reading `README.md` to understand how to interact with the module.
  - Read `DESIGN.md` to understand the internal mechanisms, structures, and assumptions of the module before starting development.
  - Avoid defaulting to auditing raw source files.

- **Justify Source Code Audits & Update Docs**:
  - If you find that the documentation (`README.md` or `DESIGN.md`) lacks necessary details and you are forced to inspect raw implementation source code, you must explicitly explain in your response why you had to read the code.
  - You **must** also update the module's `README.md` (if the missing details concern external usage/APIs) or `DESIGN.md` (if they concern internal implementation) with your findings so that future agents do not have to perform the same raw source code lookup.

- **Mandatory Updates & Documentation Creation**:
  - **Create Missing Docs**: If you are working on a module that is missing its `README.md` (for external usage) or `DESIGN.md` (for internal design details), you **must** create these missing files.
  - **Append to References**: Whenever you create or add a new `README.md` or `DESIGN.md` to a module, you **must** append its link to the **Key References in the Project** section of this guideline file.
  - **For Public Interface Changes**: If your changes modify the public APIs, usage patterns, or configuration, you must update `README.md` (or create it if it doesn't exist).
  - **For Implementation Changes**: If you modify the internal structure, protocols, state machines, or discover details that were previously undocumented, you **must** update `DESIGN.md` (or create it if it doesn't exist) before finishing development. This ensures future agents can read it to quickly catch up.
  - **Capture Insights**: Any internal insights gained from raw source code auditing should be captured and written back into `DESIGN.md`.

---

## Key References in the Project

- **libs/omni-fiber**:
  - [libs/omni-fiber/README.md](../../libs/omni-fiber/README.md) - Public API guidelines (concurrency, fiber lifecycle, joining policies).
  - [libs/omni-fiber/DESIGN.md](../../libs/omni-fiber/DESIGN.md) - Internal design specs (scheduling, state machines, symmetric transfer).
- **src/base**:
  - [src/base/README.md](../../src/base/README.md) - ServiceBase subclassing interface and thread-safety details.
- **src/core**:
  - [src/core/README.md](../../src/core/README.md) - Public APIs and abstractions for the core network modules including Endpoints, Pipelines, Filters, and PacketBuilder.
  - [src/core/DESIGN.md](../../src/core/DESIGN.md) - Internal architectures, dynamic multiplexing protocol, state machines, and implementation details.

