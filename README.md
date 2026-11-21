# Shopping Sessions

A C++20 solution to the Databricks Shopping Sessions challenge. The executable reads the mounted CSV files and manifest, then writes `/output/user_daily/` and `/output/campaign_daily/` as Parquet. The runtime contains the executable, Apache Arrow, Parquet, and their shared libraries.

## Submit

1. Extract the ZIP. Create a public GitHub repository and upload the files from inside the extracted folder. Its top level should show `Dockerfile` beside `main.cpp`.
2. Open the Shopping Sessions submission form, paste the repository URL and optionally enter the full commit hash.
3. Submit the repository. The organizer checks the public dev dataset before timing the hidden practice run.

[Challenge and submission form](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions)

## Run locally

From the directory containing the Dockerfile, run the commands below. Put a challenge dataset in the adjacent `dev` folder first. It should contain `manifest.json`, `products.csv`, `campaigns.csv`, and the `events`, `links`, and `clicks` directories. The `--user` argument makes the output files writable by your local account. The image defaults to UID 1000 on the scorer.

```sh
docker buildx build --platform linux/amd64 --load -t shopping-sessions .
mkdir -p ../out ../scratch
docker run --rm --platform linux/amd64 --network none --read-only \
  --user "$(id -u):$(id -g)" --tmpfs /tmp:size=64m \
  -v "$PWD/../dev:/data:ro" -v "$PWD/../out:/output" \
  -v "$PWD/../scratch:/scratch" shopping-sessions
```

## How it works

- **Linked users.** Union-find resolves transitive links across all files. Each connected component keeps its smallest UTF-8 byte string, including IDs found only in the links.
- **Events.** A parallel first pass identifies IDs that might repeat. For each candidate, an exact comparison selects the earliest well-formed copy across the entire dataset. An invalid winning copy discards the event. Hash collisions only send more rows through the exact comparison.
- **Sessions.** The surviving events are partitioned by person and sorted by timestamp. A gap strictly greater than 30 minutes starts a new session. Its UTC start day receives all its events and revenue.
- **Credit and assists.** A sliding seven-day window tracks eligible clicks. The latest click wins credit, with the smallest click ID breaking timestamp ties. A count for each active campaign gives one assist per campaign per session, including the winner.
- **Daily results.** Each person and day gets one row. Purchase revenue determines the top category, with byte-order tie breaking. Per-worker campaign totals are merged before writing the campaign table.

[Official rules](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions/files/rules.md?inline=1)

## Reproduce the dev check

Download a starter, the generator, and the dev answers from the challenge page. The generator takes `--seed 1 --events 1000000 --clicks 250000 --output dev`. After running the image on this input, compare both Parquet folders using the starter's script from an environment with Polars installed.

```sh
python3 path/to/starter-duckdb/scripts/compare.py ../out path/to/dev-answer
```

The official dev answer has 117,038 user-day rows and 27,400 campaign-day rows. The comparator checks every column and detects duplicate keys.

## Verification

The packaged executable passed all 36 official fixtures. Both dev outputs matched the official CSVs exactly, with zero missing rows, extra rows, or duplicate keys.

| Local input | Clicks | Elapsed time | Peak process RSS |
| --- | ---: | ---: | ---: |
| 1 million events, seed 1 | 250,000 | 0.20 s | 0.18 GiB |
| 20 million events, seed 843 | 5 million | 7.57 s | 2.93 GiB |
| 60 million events, seed 7821 | 15 million | 26.77 s | 6.88 GiB |

Each timing is one local run on eight pinned logical CPUs. The binaries were built in the Dockerfile's pinned amd64 base and run from the final filesystem as UID 1000 with networking disabled and the root mounted read-only. For the 60-million-event input, the generator spilled its intermediate shards to disk to fit the local machine. This generator variant produced byte-identical files to the official generator on the million-event dev set. The larger runs were checked for matching daily session and revenue totals across the two tables. The competition determines the official time on its own hidden inputs.
