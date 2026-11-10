# MT5 Group Symbol Manager

Connect to an MT5 Manager API profile to retrieve the effective symbol settings
for symbols listed in each server group's `Symbol` rules. Exact symbol paths
appear directly; wildcard paths expand to their matching server symbols. Each
row represents one group-symbol pair. The first
column, `Group Path`, identifies the group and has the same filter as the other
columns. `Group Path`, `Symbol`, and `Path` remain fixed at the left while the
rest of the table scrolls. The table displays configured `symbols.ini` fields,
including per-order-type margin rates and quote/trade sessions. Columns that
cannot be overridden at group level are read-only.

## Excel export and import

Use **Export Excel** to save the table as `.xlsx`. Use **Browse...** to load an
edited workbook into the table. The workbook must keep the exported first
worksheet, the generated `Group Path` column, and the same unique headings
configured in `symbols.ini`; columns may be reordered. Once loaded, **Apply Changes** writes changed fields
supported at group level. A symbol may appear in more than one group;
`Group Path` and `Symbol` together identify a row. The app does not create
symbols or alter global symbol settings.

Before applying, the app saves the current effective values across all groups
to a timestamped CSV in the `backup` directory beside the executable.
Group-specific overrides are saved without changing broader group rules.
During the update, **Abort** pauses after the current symbol. Confirm stops and
keeps successful changes; closing the confirmation resumes the update;
**Confirm and Restore existing symbols** restores prior per-symbol group
overrides and removes overrides created by that update. The app removes its
backup CSV files when it closes.

## Connection profiles

The app reads `connections.ini` beside the executable. On first launch it
creates an example profile; replace its server, login, and password, then
restart the app. Passwords are stored as plain text, so protect the file.

```ini
[Production]
server=mt5.example.com:443
login=123456
password=replace_me
```

## Symbol fields

The app reads `symbols.ini` beside the executable. Each line maps an SDK field
name to a table heading, for example `Description=Instrument description`.
The order of mappings sets the column order in the table and Excel export.
Remove a mapping line to omit that field. On first launch, the app creates a
file containing every supported field with its original name as the heading.

`[GroupSymbolFields]` controls the columns for settings stored in each group's
`Symbol` list. Its order and headings work the same way; `Path` is retained as
the symbol path, while `Group Path` is always shown first. Blank lines create
vertical dividers. `[GroupConstraints]` has the same
`minimum|maximum|maxChars|allowedValues` format for these fields. The SDK-backed
enum, flag, and integer bounds are prefilled where defined; blank slots are
templates for limits the SDK does not specify.

Optional `[ValueMap.<SDK field>]` sections map numeric enum values to labels,
for example `[ValueMap.BookVolumeMode]` with `0=Default`, `1=Amounts`, and
`2=Lots`. `[FlagMap.<SDK field>]` sections map zero, individual bits, or exact
combined values. Exact matches take precedence; otherwise known individual bits
display as labels joined by ` | `. `[TextMap.<SDK field>]`
sections map text codes to labels; `[TextMap.Country]` contains ISO 3166-1
alpha-3 codes such as `USA=United States`.
`[GroupValueMap.<field>]`, `[GroupFlagMap.<field>]`, and
`[GroupTextMap.<field>]` can override value labels specifically for group
settings. When no group-specific map is present, matching `ValueMap`, `FlagMap`,
or `TextMap` entries are reused.

The app keeps unmapped enum codes and unknown flag combinations numeric, and
passes unknown text values through unchanged. Excel import accepts configured
labels or codes; labels are converted back to their SDK values before applying.
The shipped and first-launch defaults include the SDK's enum, flag, and country
code mappings.

## Build

Build the x64 configuration with Visual Studio 2026 using the SDK files kept in
the project's `MT5SDK` folder. The build copies `MT5SDK\Libs\MT5APIManager64.dll`
beside the executable. XLSX import compiles the ZIP inflater from
`MT5SDK\Examples\Report\NFA.Reports\Tools\Zip`; it is linked into the
executable and adds no runtime DLL dependency.
