# pnt-edge UML

PlantUML sources for the Software Architecture Document, numbered in reading order.
Render all: `plantuml -tsvg docs/uml/*.puml` (or paste into any PlantUML viewer; GitHub renders
`.puml` through the PlantUML proxy in most Markdown previewers).

| File | Diagram | SAD section |
| --- | --- | --- |
| `01-context.puml` | Context (actors, external interfaces) | 3 |
| `02-components.puml` | Component diagram inside the process | 5 |
| `03-classes.puml` | Class diagram of `src/` | 5 |
| `04-seq-measurement-to-nmea.puml` | Sequence: serial frame → NMEA | 6.2 |
| `05-seq-command.puml` | Sequence: `$PPNT` / web command → main loop | 6.3 |
| `06-seq-software-update.puml` | Sequence: stage, apply, re-exec | 6.5 |
| `07-state-device.puml` | State machine: serial device | 6.4 |
| `08-state-runtime.puml` | State machine: Runtime and recovery ladder | 6.4 |
| `09-deployment.puml` | Deployment: G40 and bench | 7 |

Keep `03-classes.puml` and `07-state-device.puml` in step with the code; the others change only when the
architecture does.
