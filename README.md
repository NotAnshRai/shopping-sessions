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

CSV workers scan field boundaries in parallel, validate quotes, and keep the full byte value of every ID. Short ASCII IDs fit in one 64-bit key. Other strings use an exact lookup table. A portable amd64 build is included alongside the x86-64-v3 build.

The workers partition events by event ID. Each partition compares full IDs and ingest times, so an invalid early copy still suppresses a later valid one. A union-find resolves linked users while workers parse the logs. Only aliases that change the canonical name need a lookup entry in the final map.

A second partition assigns each person to one worker. The worker groups that person's events and clicks before sorting by time. A sliding seven-day window determines the winning click and distinct assisting campaigns. Daily totals use the session start date. The output tables are written as Parquet.

[Official rules](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions/files/rules.md?inline=1)

## Reproduce the dev check

Download a starter, the generator, and the dev answers from the challenge page. The generator takes `--seed 1 --events 1000000 --clicks 250000 --output dev`. After running the image on this input, compare both Parquet folders using the starter's script from an environment with Polars installed.

```sh
python3 path/to/starter-duckdb/scripts/compare.py ../out path/to/dev-answer
```

The official dev answer has 117,038 user-day rows and 27,400 campaign-day rows. The comparator checks every column and detects duplicate keys.

## Verification

The pull-request check builds the scorer image, compares both tables against all official fixtures and the one-million-event dev answers, and times the previous and optimized commits on identical generated practice data. The hidden scorer determines the competition result.
