#pragma once

// Fallback mapping configuration used when symbols.ini is first created.
inline constexpr char kDefaultSymbolValueMaps[] = R"SYMBOLMAPS(
# Optional SDK enum and flag labels. Unlisted numeric values remain numeric and unchanged.
# Map section names use the SDK source field name, not the column heading.
# ValueMap maps one exact numeric code to a label. FlagMap maps 0 or individual bits;
# combined known flags display as labels joined by " | ". Unknown bits remain numeric.
[ValueMap.Sector]
0=Undefined
1=Basic Materials
2=Communication Services
3=Consumer Cyclical
4=Consumer Defensive
5=Energy
6=Financial
7=Healthcare
8=Industrials
9=Real Estate
10=Technology
11=Utilities
12=Currency
13=Currency Crypto
14=Indexes
15=Commodities

[ValueMap.Industry]
0=Undefined
1=Agricultural Inputs
2=Aluminium
3=Building Materials
4=Chemicals
5=Coking Coal
6=Copper
7=Gold
8=Lumber Wood
9=Industrial Metals
10=Precious Metals
11=Paper
12=Silver
13=Specialty Chemicals
14=Steel
51=Advertising
52=Broadcasting
53=Gaming Multimedia
54=Entertainment
55=Internet Content
56=Publishing
57=Telecom
101=Apparel Manufacturing
102=Apparel Retail
103=Auto Manufacturers
104=Auto Parts
105=Auto Dealership
106=Department Stores
107=Footwear Accessories
108=Furnishings
109=Gambling
110=Home Improvement Retail
111=Internet Retail
112=Leisure
113=Lodging
114=Luxury Goods
115=Packaging Containers
116=Personal Services
117=Recreational Vehicles
118=Residential Construction
119=Resorts Casinos
120=Restaurants
121=Specialty Retail
122=Textile Manufacturing
123=Travel Services
151=Beverages Brewers
152=Beverages Non Alcoholic
153=Beverages Wineries
154=Confectioners
155=Discount Stores
156=Education Training
157=Farm Products
158=Food Distribution
159=Grocery Stores
160=Household Products
161=Packaged Foods
162=Tobacco
201=Oil Gas Drilling
202=Oil Gas Exploration and Production
203=Oil Gas Equipment
204=Oil Gas Integrated
205=Oil Gas Midstream
206=Oil Gas Refining
207=Thermal Coal
208=Uranium
251=Exchange Traded Fund
252=Asset Management
253=Banks Diversified
254=Banks Regional
255=Capital Markets
256=Closed End Fund Debt
257=Closed End Fund Equity
258=Closed End Fund Foreign
259=Credit Services
260=Financial Conglomerate
261=Financial Data Exchange
262=Insurance Brokers
263=Insurance Diversified
264=Insurance Life
265=Insurance Property
266=Insurance Reinsurance
267=Insurance Specialty
268=Mortgage Finance
269=Shell Companies
301=Biotechnology
302=Diagnostics Research
303=Drug Manufacturers
304=Drug Manufacturers Specialty
305=Healthcare Plans
306=Health Information
307=Medical Facilities
308=Medical Devices
309=Medical Distribution
310=Medical Instruments
311=Pharmacy Retailers
351=Aerospace Defense
352=Airlines
353=Airport Services
354=Building Products
355=Business Equipment
356=Conglomerates
357=Consulting Services
358=Electrical Equipment
359=Engineering Construction
360=Farm Heavy Machinery
361=Industrial Distribution
362=Infrastructure Operations
363=Freight Logistics
364=Marine Shipping
365=Metal Fabrication
366=Pollution Control
367=Railroads
368=Rental Leasing
369=Security Protection
370=Specialty Business Services
371=Specialty Machinery
372=Staffing Employment
373=Tools Accessories
374=Trucking
375=Waste Management
401=Real Estate Development
402=Real Estate Diversified
403=Real Estate Services
404=REIT Diversified
405=REIT Healthcare
406=REIT Hotel Motel
407=REIT Industrial
408=REIT Mortgage
409=REIT Office
410=REIT Residential
411=REIT Retail
412=REIT Specialty
451=Communication Equipment
452=Computer Hardware
453=Consumer Electronics
454=Electronic Components
455=Electronic Distribution
456=IT Services
457=Scientific Instruments
458=Semiconductor Equipment
459=Semiconductors
460=Software Application
461=Software Infrastructure
462=Solar
501=Utilities Diversified
502=Utilities Power Producers
503=Utilities Renewable
504=Utilities Regulated Electric
505=Utilities Regulated Gas
506=Utilities Regulated Water
551=Commodities Agricultural
552=Commodities Energy
553=Commodities Metals
554=Commodities Precious

[ValueMap.BookVolumeMode]
0=Default
1=Amounts
2=Lots

[ValueMap.ChartMode]
0=Bid
1=Last
255=Old

[ValueMap.TradeMode]
0=Disabled
1=Long only
2=Short only
3=Close only
4=Full

[ValueMap.CalcMode]
0=Forex
1=Futures
2=CFD
3=CFD index
4=CFD leverage
5=Forex no leverage
32=Exchange stocks
33=Exchange futures
34=Exchange futures FORTS
35=Exchange options
36=Exchange options margin
37=Exchange bonds
38=Exchange stocks MOEX
39=Exchange bonds MOEX
64=Collateral

[ValueMap.ExecMode]
0=Request
1=Instant
2=Market
3=Exchange

[ValueMap.GTCMode]
0=GTC
1=Daily
2=Daily no stops

[ValueMap.SwapMode]
0=Disabled
1=Points
2=Symbol currency
3=Margin currency
4=Group currency
5=Interest current
6=Interest open
7=Reopen by close price
8=Reopen by bid
9=Profit currency

[ValueMap.Swap3Day]
0=Sunday
1=Monday
2=Tuesday
3=Wednesday
4=Thursday
5=Friday
6=Saturday
7=Disabled

[ValueMap.IECheckMode]
0=Normal

[ValueMap.OptionsMode]
0=European call
1=European put
2=American call
3=American put

[ValueMap.SpliceType]
0=None
1=Unadjusted
2=Adjusted

[ValueMap.SpliceTimeType]
0=Expiration

[FlagMap.TickFlags]
0=None
1=Realtime
2=Collect raw
4=Feed stats
8=Negative prices

[FlagMap.FillFlags]
0=None
1=FOK
2=IOC
4=BOC

[FlagMap.ExpirFlags]
0=None
1=GTC
2=Day
4=Specified
8=Specified day

[FlagMap.OrderFlags]
0=None
1=Market
2=Limit
4=Stop
8=Stop limit
16=SL
32=TP
64=Close by

[FlagMap.MarginFlags]
0=None
1=Check process
2=Check SL/TP
4=Hedge large leg
8=Exclude P/L
16=Recalculate rates

[FlagMap.REFlags]
0=None
1=Order confirmation

[FlagMap.IEFlags]
0=None
1=Fast confirmation

[FlagMap.TradeFlags]
0=None
1=Profit by market
2=Allow signals

[FlagMap.SwapFlags]
0=None
1=Consider holidays
)SYMBOLMAPS";
