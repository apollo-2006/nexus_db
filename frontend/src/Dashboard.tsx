import React, { useState, useEffect } from 'react';
import { Database, Activity, HardDrive, Search, Zap, Send } from 'lucide-react';

const API_BASE = "http://localhost:8000/api";

export default function Dashboard() {
    const [metrics, setMetrics] = useState({ reads: 0, writes: 0, keys_tracked: 0, uptime_seconds: 0, recent_keys: [] });
    const [queryKey, setQueryKey] = useState("");
    const [queryResult, setQueryResult] = useState<any>(null);
    const [insertKey, setInsertKey] = useState("");
    const [insertVal, setInsertVal] = useState("");

    useEffect(() => {
        const fetchMetrics = () => {
            fetch(`${API_BASE}/metrics`)
                .then(res => res.json())
                .then(data => setMetrics(data))
                .catch(err => console.error("API Offline"));
        };
        fetchMetrics();
        const interval = setInterval(fetchMetrics, 1000);
        return () => clearInterval(interval);
    }, []);

    const handleQuery = async (e: React.FormEvent) => {
        e.preventDefault();
        try {
            const res = await fetch(`${API_BASE}/get/${queryKey}`);
            if (res.ok) {
                const data = await res.json();
                setQueryResult(data);
            } else {
                setQueryResult({ error: "Key not found in SSTables or MemTable." });
            }
        } catch (e) {
            setQueryResult({ error: "Network error." });
        }
    };

    const handleInsert = async (e: React.FormEvent) => {
        e.preventDefault();
        await fetch(`${API_BASE}/put`, {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ key: insertKey, value: insertVal })
        });
        setInsertKey("");
        setInsertVal("");
    };

    return (
        <div className="min-h-screen bg-neutral-950 text-neutral-200 p-8 font-sans">
            <header className="mb-8 flex items-center gap-3 border-b border-neutral-800 pb-4">
                <Database className="text-emerald-500" size={32} />
                <div>
                    <h1 className="text-2xl font-bold tracking-tight text-white">NexusDB Explorer</h1>
                    <p className="text-sm text-neutral-500">LSM-Tree Storage Engine Dashboard</p>
                </div>
            </header>

            <div className="grid grid-cols-1 md:grid-cols-4 gap-4 mb-8">
                <MetricCard icon={<Zap />} title="Total Writes" value={metrics.writes} color="text-yellow-500" />
                <MetricCard icon={<Search />} title="Total Reads" value={metrics.reads} color="text-blue-500" />
                <MetricCard icon={<HardDrive />} title="Keys Tracked" value={metrics.keys_tracked} color="text-purple-500" />
                <MetricCard icon={<Activity />} title="Uptime (s)" value={metrics.uptime_seconds} color="text-emerald-500" />
            </div>

            <div className="grid grid-cols-1 lg:grid-cols-2 gap-8">
                <div className="space-y-6">
                    <div className="bg-neutral-900 border border-neutral-800 rounded-xl p-6">
                        <h2 className="text-lg font-semibold mb-4 text-white flex items-center gap-2">
                            <Send size={18} className="text-yellow-500" /> Write to MemTable
                        </h2>
                        <form onSubmit={handleInsert} className="flex gap-2">
                            <input
                                type="text" placeholder="Key (e.g. user:101)" required
                                className="bg-neutral-800 border-neutral-700 text-white rounded-lg px-4 py-2 w-1/3 outline-none focus:border-emerald-500 transition-colors"
                                value={insertKey} onChange={e => setInsertKey(e.target.value)}
                            />
                            <input
                                type="text" placeholder="JSON Value" required
                                className="bg-neutral-800 border-neutral-700 text-white rounded-lg px-4 py-2 flex-1 outline-none focus:border-emerald-500 transition-colors"
                                value={insertVal} onChange={e => setInsertVal(e.target.value)}
                            />
                            <button type="submit" className="bg-emerald-600 hover:bg-emerald-500 text-white font-medium px-6 py-2 rounded-lg transition-colors">
                                Put
                            </button>
                        </form>
                    </div>

                    <div className="bg-neutral-900 border border-neutral-800 rounded-xl p-6">
                        <h2 className="text-lg font-semibold mb-4 text-white flex items-center gap-2">
                            <Search size={18} className="text-blue-500" /> Read from Engine
                        </h2>
                        <form onSubmit={handleQuery} className="flex gap-2 mb-4">
                            <input
                                type="text" placeholder="Search by Key..." required
                                className="bg-neutral-800 border-neutral-700 text-white rounded-lg px-4 py-2 flex-1 outline-none focus:border-blue-500 transition-colors"
                                value={queryKey} onChange={e => setQueryKey(e.target.value)}
                            />
                            <button type="submit" className="bg-blue-600 hover:bg-blue-500 text-white font-medium px-6 py-2 rounded-lg transition-colors">
                                Get
                            </button>
                        </form>

                        {queryResult && (
                            <div className="bg-neutral-950 rounded-lg p-4 font-mono text-sm border border-neutral-800">
                                {queryResult.error ? (
                                    <span className="text-red-400">{queryResult.error}</span>
                                ) : (
                                    <>
                                        <div className="text-neutral-500 mb-2">
                                            Latency: <span className="text-emerald-400">{queryResult.latency_us}µs</span>
                                        </div>
                                        <div className="text-emerald-300">{queryResult.value}</div>
                                    </>
                                )}
                            </div>
                        )}
                    </div>
                </div>

                <div className="bg-neutral-900 border border-neutral-800 rounded-xl p-6">
                    <h2 className="text-lg font-semibold mb-4 text-white">Recent Key Explorer</h2>
                    <div className="space-y-2">
                        {metrics.recent_keys.length === 0 ? (
                            <p className="text-neutral-500 italic text-sm">No keys inserted yet.</p>
                        ) : (
                            metrics.recent_keys.map((k: string, i: number) => (
                                <div key={i} onClick={() => setQueryKey(k)} className="bg-neutral-950 border border-neutral-800 px-4 py-2 rounded-lg text-sm font-mono hover:border-emerald-500 cursor-pointer transition-colors flex justify-between items-center">
                                    <span className="text-neutral-300">{k}</span>
                                    <span className="text-xs text-neutral-600 text-right">Click to query</span>
                                </div>
                            ))
                        )}
                    </div>
                </div>
            </div>
        </div>
    );
}

const MetricCard = ({ icon, title, value, color }: any) => (
    <div className="bg-neutral-900 border border-neutral-800 rounded-xl p-5 flex items-center gap-4">
        <div className={`p-3 bg-neutral-950 rounded-lg ${color}`}>
            {icon}
        </div>
        <div>
            <p className="text-neutral-500 text-sm font-medium">{title}</p>
            <p className="text-2xl font-bold text-white">{value}</p>
        </div>
    </div>
);