# MT5 Symbol Manager

Connect to an MT5 Manager API profile to retrieve symbol configuration and
display it in a wide, sortable table. Each row represents one symbol. Columns
cover the `IMTConSymbol` getters, including per-order-type margin rates and
quote/trade sessions for each weekday. Use the horizontal scrollbar to reach
the remaining parameters.

## Excel export and import

Use **Export Excel** to save the table as `.xlsx`. Use **Browse...** to load an
edited workbook into the table. The workbook must keep the exported first
worksheet and the same unique headings configured in `symbols.ini`; columns
may be reordered. Once loaded, **Import** applies the values to symbols that
already exist on the selected server. Unknown symbols are skipped; the app does
not create symbols.

Before applying, the app saves all current symbol values to a timestamped CSV
in the `backup` directory beside the executable. During the update, **Abort**
pauses after the current symbol. Confirm stops and keeps successful changes;
closing the confirmation resumes the import; **Confirm and Restore existing
symbols** restores every symbol touched by that import. The app removes its
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

Optional `[ValueMap.<SDK field>]` sections map numeric enum values to labels,
for example `[ValueMap.BookVolumeMode]` with `0=Default`, `1=Amounts`, and
`2=Lots`. `[FlagMap.<SDK field>]` sections map `0` and individual flag bits;
combined known flags display as labels joined by ` | `.

The app keeps unlisted enum codes and unknown flag combinations numeric, and
accepts them unchanged. Excel import accepts configured labels or numeric
values; labels are converted back to their SDK codes before applying. The
shipped and first-launch defaults cover the SDK's enum and flag fields.

## Build

Build the x64 configuration with Visual Studio 2026 and the MetaTrader 5 SDK
installed at `C:\Program Files\MetaTrader5SDK`. The build copies only the
native `MT5APIManager64.dll` beside the executable. XLSX import also compiles
the ZIP inflater from the SDK's `Examples\Report\NFA.Reports\Tools\Zip` folder;
it is linked into the executable and adds no runtime DLL dependency.
