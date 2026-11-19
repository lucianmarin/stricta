import asyncio

async def worker(tid: int, steps: int) -> int:
    acc = tid
    for i in range(steps):
        acc = (acc * 1103515245 + 12345 + i) & 2147483647
        await asyncio.sleep(0)
    return acc

async def run() -> None:
    n = 800
    steps = 2000
    results = await asyncio.gather(*(worker(i + 1, steps) for i in range(n)))
    print(sum(results))

asyncio.run(run())
