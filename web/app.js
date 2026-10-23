/**
 * DistriKV Multi-Page Dashboard
 * Client-side router connecting 5 pages to real C++ Berkeley Sockets backend.
 */

class DistriKvDashboard {
    constructor() {
        this.apiBase = '';
        this.autoRefresh = true;
        this.refreshIntervalMs = 2000;
        this.refreshTimer = null;
        this.allKeys = [];
        this.clusterState = null;
        this.lastLeaderId = null;
        this.currentPage = 'dashboard';
        this.kvPage = 1;
        this.kvPageSize = 20;
        this.termHistory = [];
        this.termHistIdx = -1;
        this.globalEventLog = [];

        this.initRouter();
        this.initDom();
        this.bindEvents();
        this.startSync();
        this.logEvent('Dashboard initialized — connecting to cluster…', 'highlight');
    }

    // ══════════════════════════════════════════════
    // ROUTER
    // ══════════════════════════════════════════════
    initRouter() {
        const hash = window.location.hash.replace('#', '') || 'dashboard';
        this.navigateTo(hash, false);
        window.addEventListener('hashchange', () => {
            const page = window.location.hash.replace('#', '') || 'dashboard';
            this.navigateTo(page, false);
        });
    }

    navigateTo(page, updateHash = true) {
        const validPages = ['dashboard', 'explorer', 'topology', 'terminal', 'architecture'];
        if (!validPages.includes(page)) page = 'dashboard';

        this.currentPage = page;
        if (updateHash) window.location.hash = page;

        // Toggle page visibility
        document.querySelectorAll('.page').forEach(p => p.classList.remove('active'));
        const target = document.getElementById(`page-${page}`);
        if (target) target.classList.add('active');

        // Toggle nav items
        document.querySelectorAll('.nav-item').forEach(n => n.classList.remove('active'));
        const navItem = document.getElementById(`nav-${page}`);
        if (navItem) navItem.classList.add('active');

        // Page-specific init
        if (page === 'explorer') {
            this.fetchKeys();
        } else if (page === 'topology') {
            if (this.clusterState) this.renderTopologyPage(this.clusterState);
        } else if (page === 'architecture') {
            if (this.clusterState) this.updateArchPage(this.clusterState);
        }

        // Scroll to top
        const main = document.getElementById('main-content');
        if (main) main.scrollTop = 0;
    }

    // ══════════════════════════════════════════════
    // DOM REFS
    // ══════════════════════════════════════════════
    initDom() {
        this.sidebarHealthPill = document.getElementById('sidebar-health-pill');
        this.sidebarHealthText = document.getElementById('sidebar-health-text');
        this.sfLeader = document.getElementById('sf-leader');
        this.sfTerm   = document.getElementById('sf-term');
        this.sfUptime = document.getElementById('sf-uptime');

        this.kpiNodes = document.getElementById('kpi-nodes-val');
        this.kpiKeys  = document.getElementById('kpi-keys-val');
        this.kpiOps   = document.getElementById('kpi-ops-val');
        this.kpiTerm  = document.getElementById('kpi-term-val');

        this.dashNodesContainer = document.getElementById('dash-nodes-container');
        this.dashEventFeed      = document.getElementById('dash-event-feed');

        this.kvTableBody     = document.getElementById('kv-table-body');
        this.kvCountBadge    = document.getElementById('kv-count-badge');
        this.filterKeysInput = document.getElementById('filter-keys-input');
        this.kvNsFilter      = document.getElementById('kv-namespace-filter');
        this.kvShowingText   = document.getElementById('kv-showing-text');
        this.kvPagination    = document.getElementById('kv-pagination');

        this.topoContainer = document.getElementById('topo-nodes-container');
        this.topoDiagram   = document.getElementById('topo-diagram');
        this.topoPeerSummary = document.getElementById('topo-peer-summary');

        this.terminalScreen = document.getElementById('terminal-screen');
        this.terminalInput  = document.getElementById('terminal-input');
        this.logsFeed       = document.getElementById('logs-feed');

        this.benchModal     = document.getElementById('bench-modal');
        this.benchModalBody = document.getElementById('bench-modal-body');
        this.autoRefreshToggle = document.getElementById('auto-refresh-toggle');

        this.navKeysBadge  = document.getElementById('nav-keys-badge');
        this.navNodesBadge = document.getElementById('nav-nodes-badge');
    }

    // ══════════════════════════════════════════════
    // EVENT BINDINGS
    // ══════════════════════════════════════════════
    bindEvents() {
        // Sidebar nav
        document.querySelectorAll('.nav-item[data-page]').forEach(link => {
            link.addEventListener('click', (e) => {
                e.preventDefault();
                this.navigateTo(link.dataset.page);
            });
        });
        // "View Full Topology" / "Full Logs" etc links
        document.querySelectorAll('[data-page]').forEach(el => {
            if (!el.classList.contains('nav-item')) {
                el.addEventListener('click', (e) => {
                    e.preventDefault();
                    this.navigateTo(el.dataset.page);
                });
            }
        });

        // Auto refresh toggle
        this.autoRefreshToggle.addEventListener('change', (e) => {
            this.autoRefresh = e.target.checked;
            if (this.autoRefresh) { this.startSync(); this.logEvent('Live sync enabled.'); }
            else { this.stopSync(); this.logEvent('Live sync paused.', 'warn'); }
        });

        // Dashboard buttons
        document.getElementById('btn-refresh').addEventListener('click', () => this.refresh());
        document.getElementById('btn-chaos-failover').addEventListener('click', () => this.triggerChaosFailover());
        document.getElementById('btn-run-bench').addEventListener('click', () => this.runBenchmarkModal());
        document.getElementById('btn-close-modal').addEventListener('click', () => this.benchModal.classList.add('hidden'));

        // Dashboard quick-put
        document.getElementById('dash-put-form').addEventListener('submit', (e) => {
            e.preventDefault(); this.handleDashPut();
        });

        // KV Explorer
        document.getElementById('kv-insert-form').addEventListener('submit', (e) => {
            e.preventDefault(); this.handleQuickPut();
        });
        document.getElementById('btn-kv-refresh').addEventListener('click', () => this.fetchKeys());
        this.filterKeysInput.addEventListener('input', () => { this.kvPage = 1; this.renderKeysTable(); });
        this.kvNsFilter.addEventListener('change', () => { this.kvPage = 1; this.renderKeysTable(); });

        // Topology page
        document.getElementById('topo-chaos-btn').addEventListener('click', () => this.triggerChaosFailover());

        // Terminal
        document.getElementById('terminal-form').addEventListener('submit', (e) => {
            e.preventDefault(); this.handleTerminalSubmit();
        });
        this.terminalInput.addEventListener('keydown', (e) => this.handleTermHistory(e));

        // Tab buttons (terminal page)
        document.getElementById('tab-terminal-btn').addEventListener('click', () => {
            document.getElementById('tab-terminal-btn').classList.add('active');
            document.getElementById('tab-logs-btn').classList.remove('active');
        });
        document.getElementById('tab-logs-btn').addEventListener('click', () => {
            document.getElementById('tab-logs-btn').classList.add('active');
            document.getElementById('tab-terminal-btn').classList.remove('active');
        });
    }

    // ══════════════════════════════════════════════
    // SYNC LOOP
    // ══════════════════════════════════════════════
    startSync() {
        this.stopSync();
        this.refresh();
        this.refreshTimer = setInterval(() => {
            if (this.autoRefresh) this.refresh();
        }, this.refreshIntervalMs);
    }

    stopSync() {
        if (this.refreshTimer) { clearInterval(this.refreshTimer); this.refreshTimer = null; }
    }

    async refresh() {
        await this.fetchClusterState();
        if (this.currentPage === 'explorer') await this.fetchKeys();
    }

    // ══════════════════════════════════════════════
    // CLUSTER STATE
    // ══════════════════════════════════════════════
    async fetchClusterState() {
        try {
            const resp = await fetch(`${this.apiBase}/api/cluster`);
            if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
            const data = await resp.json();
            this.clusterState = data;
            this.renderAll(data);
        } catch (err) {
            this.renderDisconnected(err.message);
        }
    }

    renderAll(data) {
        this.updateSidebar(data);
        this.updateKPIs(data);
        this.updateDashNodes(data.nodes, data.leader_id);
        if (this.currentPage === 'topology') this.renderTopologyPage(data);
        if (this.currentPage === 'architecture') this.updateArchPage(data);

        // Leader change detection
        if (this.lastLeaderId !== null && data.leader_id !== this.lastLeaderId && data.leader_id > 0) {
            this.logEvent(`>>> LEADER ELECTION! Node ${data.leader_id} is now PRIMARY (Term ${data.current_term}) <<<`, 'highlight');
        }
        this.lastLeaderId = data.leader_id;
    }

    updateSidebar(data) {
        const cls = data.status === 'healthy' ? 'healthy' : (data.status === 'degraded' ? 'degraded' : 'down');
        this.sidebarHealthPill.className = `sidebar-status-pill ${cls}`;
        const labels = { healthy: `Healthy ${data.active_nodes}/${data.total_nodes}`, degraded: `Degraded ${data.active_nodes}/${data.total_nodes}`, down: 'Cluster Down' };
        this.sidebarHealthText.textContent = labels[data.status] || 'Unknown';
        this.sfLeader.textContent = data.leader_id > 0 ? `Node ${data.leader_id}` : '—';
        this.sfTerm.textContent = data.current_term || 0;

        // Uptime from primary node
        const primary = data.nodes.find(n => n.role === 'PRIMARY');
        if (primary) {
            const up = primary.uptime_seconds || 0;
            const h = Math.floor(up / 3600), m = Math.floor((up % 3600) / 60), s = up % 60;
            this.sfUptime.textContent = h > 0 ? `${h}h ${m}m` : m > 0 ? `${m}m ${s}s` : `${s}s`;
        }

        this.navNodesBadge.textContent = data.active_nodes;
        this.navNodesBadge.className = `nav-badge nodes-badge ${data.status}`;
    }

    updateKPIs(data) {
        this.kpiNodes.textContent = `${data.active_nodes} / ${data.total_nodes}`;
        this.kpiTerm.textContent = `Term ${data.current_term || 0}`;

        let totalOps = data.total_client_ops || 0;
        let totalKeys = 0;
        data.nodes.forEach(n => {
            if (data.total_client_ops === undefined) totalOps += (n.client_ops || 0);
            if (n.role === 'PRIMARY') totalKeys = n.total_keys || 0;
        });
        this.kpiOps.textContent = totalOps.toLocaleString();
        this.kpiKeys.textContent = totalKeys.toLocaleString();
        this.navKeysBadge.textContent = totalKeys;
    }

    // ══════════════════════════════════════════════
    // DASHBOARD NODES (compact cards)
    // ══════════════════════════════════════════════
    updateDashNodes(nodes, leaderId) {
        this.dashNodesContainer.innerHTML = '';
        nodes.forEach(node => {
            const isAlive = node.alive;
            const isPrimary = node.role === 'PRIMARY';
            const roleClass = !isAlive ? 'offline' : isPrimary ? 'primary' : 'secondary';
            const roleText = !isAlive ? 'OFFLINE' : isPrimary ? 'PRIMARY' : 'SECONDARY';

            const card = document.createElement('div');
            card.className = `node-card glass-card ${roleClass}`;
            card.innerHTML = `
                <div class="node-header">
                    <div class="node-title-group">
                        <div class="node-avatar">${node.id}</div>
                        <div>
                            <div class="node-name">Node ${node.id}</div>
                            <div class="node-endpoint">${node.host}:${node.tcp_port}</div>
                        </div>
                    </div>
                    <span class="role-badge ${roleClass}">${roleText}</span>
                </div>
                <div class="node-stats-grid">
                    <div class="node-stat-item"><span class="label">UDP HB Port</span><span class="val">${node.heartbeat_port}</span></div>
                    <div class="node-stat-item"><span class="label">Term</span><span class="val">${node.term || 0}</span></div>
                    <div class="node-stat-item"><span class="label">Keys</span><span class="val">${node.total_keys || 0}</span></div>
                    <div class="node-stat-item"><span class="label">Mem</span><span class="val">${this.formatBytes(node.memory_bytes || 0)}</span></div>
                    <div class="node-stat-item"><span class="label">Log Index</span><span class="val">${node.last_log_index || 0}</span></div>
                    <div class="node-stat-item"><span class="label">P/G/D Ops</span><span class="val">${node.put_count||0}/${node.get_count||0}/${node.del_count||0}</span></div>
                </div>
                <div class="node-footer">
                    <div class="node-hb-pulse">
                        <span class="status-pulse-dot" style="color:${isAlive ? '#10b981' : '#f43f5e'}"></span>
                        <span>${isAlive ? 'Heartbeat Active' : 'Unreachable'}</span>
                    </div>
                    <div>
                        ${isAlive
                            ? `<button class="btn btn-danger btn-sm" onclick="dashboard.killNode(${node.id})">Kill</button>`
                            : `<button class="btn btn-success btn-sm" onclick="dashboard.startNode(${node.id})">Restart</button>`}
                    </div>
                </div>
            `;
            this.dashNodesContainer.appendChild(card);
        });
    }

    renderDisconnected(msg) {
        this.sidebarHealthPill.className = 'sidebar-status-pill down';
        this.sidebarHealthText.textContent = 'Disconnected';
        this.kpiNodes.textContent = '0 / 3';
    }

    // ══════════════════════════════════════════════
    // KV EXPLORER
    // ══════════════════════════════════════════════
    async fetchKeys() {
        try {
            const resp = await fetch(`${this.apiBase}/api/keys`);
            if (!resp.ok) return;
            const data = await resp.json();
            if (data.success && data.keys) {
                this.allKeys = data.keys;
                this.navKeysBadge.textContent = data.count;
                this.renderKeysTable();
            }
        } catch (e) { /* retry next tick */ }
    }

    renderKeysTable() {
        const pattern = this.filterKeysInput.value.toLowerCase().trim();
        const ns      = this.kvNsFilter ? this.kvNsFilter.value : '';
        let filtered  = this.allKeys.filter(k => {
            const key = k.key.toLowerCase();
            return (!pattern || key.includes(pattern)) && (!ns || k.key.startsWith(ns));
        });

        this.kvCountBadge.textContent = `${this.allKeys.length} Keys`;

        const total = filtered.length;
        const totalPages = Math.max(1, Math.ceil(total / this.kvPageSize));
        if (this.kvPage > totalPages) this.kvPage = totalPages;

        const start = (this.kvPage - 1) * this.kvPageSize;
        const pageItems = filtered.slice(start, start + this.kvPageSize);

        this.kvShowingText.textContent = total === 0 ? 'No keys found' : `Showing ${start+1}–${Math.min(start+this.kvPageSize, total)} of ${total} keys`;

        if (pageItems.length === 0) {
            this.kvTableBody.innerHTML = `<tr><td colspan="5" class="empty-state">${pattern||ns ? 'No keys match the filter.' : 'No keys in database.'}</td></tr>`;
            this.kvPagination.innerHTML = '';
            return;
        }

        this.kvTableBody.innerHTML = pageItems.map(item => {
            const ns = this.detectNs(item.key);
            return `
            <tr>
                <td><span class="key-code">${this.escHtml(item.key)}</span></td>
                <td><span class="val-code">${this.escHtml(item.value)}</span></td>
                <td><span class="text-dim mono11">${item.size_bytes} B</span></td>
                <td><span class="ns-pill ns-${ns}">${ns}</span></td>
                <td>
                    <div class="action-btns">
                        <button class="btn-icon-xs" title="GET value" onclick="dashboard.getKeyInTerminal('${this.escHtml(item.key)}')">GET</button>
                        <button class="btn-icon-xs danger" title="Delete key" onclick="dashboard.deleteKey('${this.escHtml(item.key)}')">DEL</button>
                    </div>
                </td>
            </tr>`;
        }).join('');

        // Pagination
        this.renderPagination(totalPages);
    }

    detectNs(key) {
        const parts = key.split(':');
        return parts.length > 1 ? parts[0] : 'other';
    }

    renderPagination(totalPages) {
        if (totalPages <= 1) { this.kvPagination.innerHTML = ''; return; }
        let html = '';
        if (this.kvPage > 1) html += `<button class="pg-btn" onclick="dashboard.goPage(${this.kvPage-1})">‹</button>`;
        const range = 2;
        for (let i = Math.max(1, this.kvPage-range); i <= Math.min(totalPages, this.kvPage+range); i++) {
            html += `<button class="pg-btn ${i===this.kvPage ? 'active':''}" onclick="dashboard.goPage(${i})">${i}</button>`;
        }
        if (this.kvPage < totalPages) html += `<button class="pg-btn" onclick="dashboard.goPage(${this.kvPage+1})">›</button>`;
        this.kvPagination.innerHTML = html;
    }

    goPage(n) { this.kvPage = n; this.renderKeysTable(); }

    // ══════════════════════════════════════════════
    // TOPOLOGY PAGE
    // ══════════════════════════════════════════════
    renderTopologyPage(data) {
        this.renderTopoDiagram(data);
        this.renderTopoNodes(data.nodes, data.leader_id);
        const alive = data.nodes.filter(n => n.alive).length;
        this.topoPeerSummary.textContent = `${alive} / ${data.total_nodes} nodes online · Leader: Node ${data.leader_id > 0 ? data.leader_id : '?'} · Term ${data.current_term || 0}`;
    }

    renderTopoDiagram(data) {
        const nodes = data.nodes;
        const leader = nodes.find(n => n.role === 'PRIMARY');
        const followers = nodes.filter(n => n.role !== 'PRIMARY');

        const colors = { PRIMARY: '#00f2fe', SECONDARY: '#10b981', OFFLINE: '#f43f5e', CANDIDATE: '#f59e0b' };
        const getColor = (n) => n.alive ? colors[n.role] || '#64748b' : colors.OFFLINE;

        let html = `<svg viewBox="0 0 700 320" xmlns="http://www.w3.org/2000/svg" style="width:100%;height:100%">
          <defs>
            <marker id="arrowhead" markerWidth="8" markerHeight="6" refX="8" refY="3" orient="auto">
              <polygon points="0 0, 8 3, 0 6" fill="rgba(255,255,255,0.25)"/>
            </marker>
            <marker id="arrowhead-rep" markerWidth="8" markerHeight="6" refX="8" refY="3" orient="auto">
              <polygon points="0 0, 8 3, 0 6" fill="#00f2fe"/>
            </marker>
            <filter id="glow"><feGaussianBlur stdDeviation="3" result="blur"/><feMerge><feMergeNode in="blur"/><feMergeNode in="SourceGraphic"/></feMerge></filter>
          </defs>`;

        // Client node at top
        html += `
          <g transform="translate(350,30)">
            <rect x="-70" y="-18" width="140" height="36" rx="8" fill="rgba(255,255,255,0.05)" stroke="rgba(255,255,255,0.15)" stroke-width="1"/>
            <text x="0" y="5" text-anchor="middle" font-family="JetBrains Mono,monospace" font-size="11" fill="#94a3b8">CLI Client / Web Dashboard</text>
          </g>`;

        // Arrow from client to primary
        if (leader) {
            html += `<line x1="350" y1="48" x2="350" y2="105" stroke="rgba(255,255,255,0.2)" stroke-width="1.5" stroke-dasharray="4,3" marker-end="url(#arrowhead)"/>
            <text x="358" y="82" font-family="Inter,sans-serif" font-size="9" fill="#64748b">TCP PUT/GET/DEL</text>`;
        }

        // Node positions
        const positions = nodes.length === 3
            ? [[350, 145], [160, 260], [540, 260]]
            : nodes.map((_, i) => [170 + i * 180, 200]);

        nodes.forEach((node, idx) => {
            const [cx, cy] = positions[idx];
            const color = getColor(node);
            const isPrimary = node.role === 'PRIMARY' && node.alive;

            // Glow ring for primary
            if (isPrimary) {
                html += `<circle cx="${cx}" cy="${cy}" r="42" fill="none" stroke="${color}" stroke-width="1" opacity="0.3"/>
                         <circle cx="${cx}" cy="${cy}" r="46" fill="none" stroke="${color}" stroke-width="0.5" opacity="0.15"/>`;
            }

            // Node circle
            html += `<circle cx="${cx}" cy="${cy}" r="36" fill="rgba(15,22,40,0.9)" stroke="${color}" stroke-width="${isPrimary ? 2.5 : 1.5}" ${isPrimary ? 'filter="url(#glow)"' : ''}/>`;

            // ID label
            html += `<text x="${cx}" y="${cy-6}" text-anchor="middle" font-family="Outfit,sans-serif" font-size="16" font-weight="700" fill="${color}">N${node.id}</text>`;

            // Role label
            const roleShort = !node.alive ? 'OFFLINE' : (node.role === 'PRIMARY' ? 'PRIMARY' : 'SECONDARY');
            html += `<text x="${cx}" y="${cy+10}" text-anchor="middle" font-family="Inter,sans-serif" font-size="8" font-weight="600" fill="${color}" opacity="0.85">${roleShort}</text>`;

            // Port label below
            html += `<text x="${cx}" y="${cy+56}" text-anchor="middle" font-family="JetBrains Mono,monospace" font-size="9" fill="#64748b">:${node.tcp_port}</text>`;

            // Heartbeat label
            html += `<text x="${cx}" y="${cy+68}" text-anchor="middle" font-family="JetBrains Mono,monospace" font-size="8" fill="#475569">UDP :${node.heartbeat_port}</text>`;
        });

        // Replication arrows: primary → followers
        if (leader) {
            const leaderIdx = nodes.indexOf(leader);
            nodes.forEach((node, idx) => {
                if (node !== leader && node.alive) {
                    const [x1, y1] = positions[leaderIdx];
                    const [x2, y2] = positions[idx];
                    const mx = (x1 + x2) / 2;
                    const my = (y1 + y2) / 2;
                    const angle = Math.atan2(y2 - y1, x2 - x1);
                    const offset = 38;
                    const sx = x1 + offset * Math.cos(angle);
                    const sy = y1 + offset * Math.sin(angle);
                    const ex = x2 - offset * Math.cos(angle);
                    const ey = y2 - offset * Math.sin(angle);

                    html += `<line x1="${sx}" y1="${sy}" x2="${ex}" y2="${ey}" stroke="#00f2fe" stroke-width="1.5" opacity="0.5" marker-end="url(#arrowhead-rep)"/>`;
                    html += `<text x="${mx}" y="${my-6}" text-anchor="middle" font-family="Inter,sans-serif" font-size="8" fill="#00f2fe" opacity="0.7">TCP Replication</text>`;
                }
            });
        }

        // Heartbeat dashed between all alive nodes
        for (let i = 0; i < nodes.length; i++) {
            for (let j = i + 1; j < nodes.length; j++) {
                if (nodes[i].alive && nodes[j].alive) {
                    const [x1,y1] = positions[i], [x2,y2] = positions[j];
                    html += `<line x1="${x1}" y1="${y1}" x2="${x2}" y2="${y2}" stroke="#f59e0b" stroke-width="0.8" stroke-dasharray="3,6" opacity="0.3"/>`;
                }
            }
        }

        html += '</svg>';
        this.topoDiagram.innerHTML = html;
    }

    renderTopoNodes(nodes, leaderId) {
        this.topoContainer.innerHTML = '';
        nodes.forEach(node => {
            const isAlive = node.alive;
            const isPrimary = node.role === 'PRIMARY';
            const roleClass = !isAlive ? 'offline' : isPrimary ? 'primary' : 'secondary';
            const roleText  = !isAlive ? 'OFFLINE' : isPrimary ? 'PRIMARY' : 'SECONDARY';

            const peers = node.peers || [];
            const peersHtml = peers.map(p =>
                `<div class="peer-row">
                    <span class="peer-id">Node ${p.id}</span>
                    <span class="peer-ep">${p.host}:${p.tcp_port}</span>
                    <span class="peer-status ${p.alive ? 'alive' : 'dead'}">${p.alive ? '✓ alive' : '✗ down'}</span>
                    ${p.last_seen_ms_ago >= 0 ? `<span class="peer-lag">${p.last_seen_ms_ago}ms ago</span>` : ''}
                </div>`
            ).join('');

            const card = document.createElement('div');
            card.className = `node-card glass-card ${roleClass} topo-card`;
            card.innerHTML = `
                <div class="node-header">
                    <div class="node-title-group">
                        <div class="node-avatar">${node.id}</div>
                        <div>
                            <div class="node-name">Node ${node.id}</div>
                            <div class="node-endpoint">${node.host}:${node.tcp_port}</div>
                        </div>
                    </div>
                    <span class="role-badge ${roleClass}">${roleText}</span>
                </div>
                <div class="node-stats-grid">
                    <div class="node-stat-item"><span class="label">Heartbeat (UDP)</span><span class="val">:${node.heartbeat_port}</span></div>
                    <div class="node-stat-item"><span class="label">Raft Term</span><span class="val">${node.term || 0}</span></div>
                    <div class="node-stat-item"><span class="label">Log Index</span><span class="val">${node.last_log_index || 0}</span></div>
                    <div class="node-stat-item"><span class="label">Keys Stored</span><span class="val">${node.total_keys || 0}</span></div>
                    <div class="node-stat-item"><span class="label">Memory</span><span class="val">${this.formatBytes(node.memory_bytes || 0)}</span></div>
                    <div class="node-stat-item"><span class="label">Uptime</span><span class="val">${this.formatUptime(node.uptime_seconds || 0)}</span></div>
                    <div class="node-stat-item"><span class="label">PUT / GET / DEL</span><span class="val">${node.put_count||0} / ${node.get_count||0} / ${node.del_count||0}</span></div>
                    <div class="node-stat-item"><span class="label">Leader</span><span class="val">Node ${node.leader_id || '?'}</span></div>
                </div>
                ${peers.length > 0 ? `<div class="peers-section"><div class="peers-title">Peer View</div>${peersHtml}</div>` : ''}
                <div class="node-footer">
                    <div class="node-hb-pulse">
                        <span class="status-pulse-dot" style="color:${isAlive ? '#10b981' : '#f43f5e'}"></span>
                        <span>${isAlive ? 'Heartbeat Active' : 'Unreachable'}</span>
                    </div>
                    <div>
                        ${isAlive
                            ? `<button class="btn btn-danger btn-sm" onclick="dashboard.killNode(${node.id})">Kill Node</button>`
                            : `<button class="btn btn-success btn-sm" onclick="dashboard.startNode(${node.id})">Restart</button>`}
                    </div>
                </div>
            `;
            this.topoContainer.appendChild(card);
        });
    }

    // ══════════════════════════════════════════════
    // ARCHITECTURE PAGE
    // ══════════════════════════════════════════════
    updateArchPage(data) {
        const set = (id, v) => { const el = document.getElementById(id); if (el) el.textContent = v; };
        set('arch-status', data.status);
        set('arch-leader', data.leader_id > 0 ? `Node ${data.leader_id}` : '—');
        set('arch-term', data.current_term || 0);
        set('arch-nodes', `${data.active_nodes}/${data.total_nodes}`);
        const primary = data.nodes.find(n => n.role === 'PRIMARY');
        set('arch-keys', primary ? (primary.total_keys || 0) : 0);
        set('arch-ops', (data.total_client_ops || 0).toLocaleString());
    }

    // ══════════════════════════════════════════════
    // COMMANDS
    // ══════════════════════════════════════════════
    async handleDashPut() {
        const key = document.getElementById('dash-key').value.trim();
        const val = document.getElementById('dash-val').value.trim();
        const ttl = parseInt(document.getElementById('dash-ttl').value) || 0;
        const fb  = document.getElementById('dash-put-feedback');
        if (!key || !val) return;

        const cmd = ttl > 0 ? `PUT ${key} ${val} ${ttl}` : `PUT ${key} ${val}`;
        fb.textContent = 'Committing…';
        fb.className = 'put-feedback';

        const res = await this.sendQuery(cmd);
        if (res.success && (res.data || '').includes('OK')) {
            fb.textContent = `✓ Key "${key}" written to cluster.`;
            fb.className = 'put-feedback success';
            document.getElementById('dash-key').value = '';
            document.getElementById('dash-val').value = '';
            document.getElementById('dash-ttl').value = '';
            this.logEvent(`PUT [${key}] committed.`, 'highlight');
            this.logDash(`PUT [${key}] → ${val}`);
        } else {
            fb.textContent = `✗ ${res.error || res.data || 'Failed'}`;
            fb.className = 'put-feedback error';
        }
        setTimeout(() => { fb.textContent = ''; fb.className = 'put-feedback'; }, 4000);
        this.fetchClusterState();
    }

    async handleQuickPut() {
        const key = document.getElementById('input-key').value.trim();
        const val = document.getElementById('input-value').value.trim();
        const ttl = parseInt(document.getElementById('input-ttl').value) || 0;
        if (!key || !val) return;

        const cmd = ttl > 0 ? `PUT ${key} ${val} ${ttl}` : `PUT ${key} ${val}`;
        this.logEvent(`Executing: ${cmd}`);
        const res = await this.sendQuery(cmd);
        if (res.success) {
            this.logEvent(`PUT [${key}] committed.`, 'highlight');
            document.getElementById('input-key').value = '';
            document.getElementById('input-value').value = '';
            document.getElementById('input-ttl').value = '';
            this.fetchKeys();
        } else {
            this.logEvent(`PUT failed: ${res.error || res.data}`, 'err');
        }
    }

    async deleteKey(key) {
        if (!confirm(`Delete key "${key}" from cluster?`)) return;
        const res = await this.sendQuery(`DEL ${key}`);
        if (res.success) {
            this.logEvent(`DEL [${key}] removed.`, 'warn');
            this.fetchKeys();
        } else {
            this.logEvent(`DEL failed: ${res.error || res.data}`, 'err');
        }
    }

    getKeyInTerminal(key) {
        this.navigateTo('terminal');
        setTimeout(() => {
            this.terminalInput.value = `GET ${key}`;
            this.terminalInput.focus();
        }, 150);
    }

    async handleTerminalSubmit() {
        const cmd = this.terminalInput.value.trim();
        if (!cmd) return;

        this.termHistory.unshift(cmd);
        if (this.termHistory.length > 50) this.termHistory.pop();
        this.termHistIdx = -1;
        this.appendTerminalLine(`› ${cmd}`, 'user-line');
        this.terminalInput.value = '';

        const res = await this.sendQuery(cmd);
        if (res.redirected_to) {
            this.appendTerminalLine(`[Redirected to ${res.redirected_to}]`, 'system-line');
        }
        if (res.success) {
            const lines = (res.data || '').split('\n');
            lines.forEach(l => this.appendTerminalLine(l, 'response-line'));
            this.logEvent(`CMD: ${cmd}`);
        } else {
            this.appendTerminalLine(`Error: ${res.error || res.data}`, 'error-line');
        }
        this.fetchClusterState();
        if (this.currentPage === 'explorer') this.fetchKeys();
    }

    handleTermHistory(e) {
        if (e.key === 'ArrowUp') {
            e.preventDefault();
            if (this.termHistIdx < this.termHistory.length - 1) {
                this.termHistIdx++;
                this.terminalInput.value = this.termHistory[this.termHistIdx];
            }
        } else if (e.key === 'ArrowDown') {
            e.preventDefault();
            if (this.termHistIdx > 0) {
                this.termHistIdx--;
                this.terminalInput.value = this.termHistory[this.termHistIdx];
            } else {
                this.termHistIdx = -1;
                this.terminalInput.value = '';
            }
        }
    }

    quickCmd(cmd) {
        this.terminalInput.value = cmd;
        this.handleTerminalSubmit();
    }

    clearTerminal() {
        this.terminalScreen.innerHTML = `
            <div class="terminal-line system-line">Terminal cleared.</div>
            <div class="terminal-line system-line">Commands: PUT &lt;key&gt; &lt;val&gt; [ttl], GET &lt;key&gt;, DEL &lt;key&gt;, KEYS, STATS, CLUSTER, PING</div>`;
    }

    clearLogs() {
        this.logsFeed.innerHTML = '';
        this.globalEventLog = [];
    }

    appendTerminalLine(text, className = '') {
        const line = document.createElement('div');
        line.className = `terminal-line ${className}`;
        line.textContent = text;
        this.terminalScreen.appendChild(line);
        this.terminalScreen.scrollTop = this.terminalScreen.scrollHeight;
    }

    // ══════════════════════════════════════════════
    // NODE CONTROL
    // ══════════════════════════════════════════════
    async killNode(nodeId) {
        this.logEvent(`Sending SIGKILL to Node ${nodeId}…`, 'warn');
        try {
            const resp = await fetch(`${this.apiBase}/api/node/kill`, {
                method: 'POST', headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ id: nodeId })
            });
            const data = await resp.json();
            if (data.success) {
                this.logEvent(data.message, 'highlight');
                setTimeout(() => this.fetchClusterState(), 500);
            }
        } catch (err) { this.logEvent(`Error: ${err.message}`, 'err'); }
    }

    async startNode(nodeId) {
        this.logEvent(`Spawning Node ${nodeId} as SECONDARY…`, 'highlight');
        try {
            const resp = await fetch(`${this.apiBase}/api/node/start`, {
                method: 'POST', headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ id: nodeId })
            });
            const data = await resp.json();
            if (data.success) {
                this.logEvent(data.message, 'highlight');
                setTimeout(() => this.fetchClusterState(), 700);
            }
        } catch (err) { this.logEvent(`Error: ${err.message}`, 'err'); }
    }

    async triggerChaosFailover() {
        if (!this.clusterState || this.clusterState.leader_id <= 0) {
            alert('No active leader detected.'); return;
        }
        const lid = this.clusterState.leader_id;
        if (!confirm(`Simulate crash of Leader Node ${lid}?\nFollowers will detect timeout via UDP heartbeats and elect a new leader.`)) return;
        this.logEvent(`CHAOS TEST: Killing Leader Node ${lid}…`, 'err');
        await this.killNode(lid);
    }

    // ══════════════════════════════════════════════
    // BENCHMARK
    // ══════════════════════════════════════════════
    async runBenchmarkModal() {
        this.benchModal.classList.remove('hidden');
        this.benchModalBody.innerHTML = `
            <div class="benchmark-loading">
                <div class="spinner"></div>
                <p>Running multi-threaded stress benchmark over Berkeley Sockets…</p>
                <p style="font-size:11px;color:#64748b;margin-top:8px">2 threads × 250 mixed PUT/GET operations</p>
            </div>`;
        try {
            const resp = await fetch(`${this.apiBase}/api/benchmark`, {
                method: 'POST', headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ ops: 500 })
            });
            const data = await resp.json();
            if (data.success) {
                this.benchModalBody.innerHTML = `
                    <div style="margin-bottom:16px">
                        <h4 style="color:var(--accent-emerald);margin-bottom:6px">✓ Benchmark Complete</h4>
                        <p style="font-size:12px;color:var(--text-muted)">Concurrent PUT and GET over POSIX socket streams.</p>
                    </div>
                    <div class="bench-metric-row"><span>Total Operations</span><span class="val">${data.ops} ops</span></div>
                    <div class="bench-metric-row"><span>Elapsed Time</span><span class="val">${data.elapsed_sec} s</span></div>
                    <div class="bench-metric-row"><span>Throughput</span><span class="val" style="color:var(--accent-emerald)">${data.throughput_ops_sec.toLocaleString()} ops/sec</span></div>
                    <div class="bench-metric-row"><span>Avg Latency</span><span class="val">${data.latency_ms} ms/op</span></div>
                    <div style="margin-top:20px;text-align:right">
                        <button class="btn btn-secondary btn-sm" onclick="document.getElementById('bench-modal').classList.add('hidden')">Close</button>
                    </div>`;
                this.logEvent(`Benchmark: ${data.throughput_ops_sec} ops/sec @ ${data.latency_ms}ms`, 'highlight');
                this.fetchClusterState();
            } else {
                this.benchModalBody.innerHTML = `<p style="color:var(--accent-pink)">Benchmark failed: ${data.error}</p>`;
            }
        } catch (err) {
            this.benchModalBody.innerHTML = `<p style="color:var(--accent-pink)">Request error: ${err.message}</p>`;
        }
    }

    // ══════════════════════════════════════════════
    // API HELPER
    // ══════════════════════════════════════════════
    async sendQuery(cmd) {
        try {
            const resp = await fetch(`${this.apiBase}/api/query`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ command: cmd })
            });
            const res = await resp.json();
            // Auto-follow MOVED redirects
            if (res.success && (res.data || '').startsWith('MOVED ')) {
                const ep = res.data.split(' ')[1];
                const [rh, rp] = ep.split(':');
                return await this.sendQueryTo(rh, parseInt(rp), cmd);
            }
            return res;
        } catch (err) {
            return { success: false, error: err.message };
        }
    }

    async sendQueryTo(host, port, cmd) {
        try {
            const resp = await fetch(`${this.apiBase}/api/query`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ command: cmd, host, port })
            });
            return await resp.json();
        } catch (err) { return { success: false, error: err.message }; }
    }

    // ══════════════════════════════════════════════
    // LOGGING
    // ══════════════════════════════════════════════
    logEvent(message, style = '') {
        const timeStr = new Date().toLocaleTimeString();
        const entry = document.createElement('div');
        entry.className = 'log-entry';
        entry.innerHTML = `<span class="log-time">[${timeStr}]</span><span class="log-msg ${style}">${this.escHtml(message)}</span>`;
        this.logsFeed.appendChild(entry);
        this.logsFeed.scrollTop = this.logsFeed.scrollHeight;
        this.globalEventLog.push({ time: timeStr, message, style });
        // Mirror to dashboard mini-feed
        this.logDash(message, style);
    }

    logDash(message, style = '') {
        if (!this.dashEventFeed) return;
        const timeStr = new Date().toLocaleTimeString();
        const entry = document.createElement('div');
        entry.className = 'log-entry';
        entry.innerHTML = `<span class="log-time">[${timeStr}]</span><span class="log-msg ${style}">${this.escHtml(message)}</span>`;
        this.dashEventFeed.appendChild(entry);
        this.dashEventFeed.scrollTop = this.dashEventFeed.scrollHeight;
        // Keep dashboard feed trim
        while (this.dashEventFeed.children.length > 30) {
            this.dashEventFeed.removeChild(this.dashEventFeed.firstChild);
        }
    }

    // ══════════════════════════════════════════════
    // UTILITIES
    // ══════════════════════════════════════════════
    formatBytes(bytes) {
        if (!bytes || bytes === 0) return '0 B';
        const k = 1024, s = ['B','KB','MB','GB'];
        const i = Math.floor(Math.log(bytes) / Math.log(k));
        return parseFloat((bytes / Math.pow(k, i)).toFixed(1)) + ' ' + s[i];
    }

    formatUptime(s) {
        const h = Math.floor(s/3600), m = Math.floor((s%3600)/60), ss = s%60;
        return h > 0 ? `${h}h${m}m` : m > 0 ? `${m}m${ss}s` : `${ss}s`;
    }

    escHtml(str) {
        if (!str) return '';
        return String(str)
            .replace(/&/g,'&amp;').replace(/</g,'&lt;')
            .replace(/>/g,'&gt;').replace(/"/g,'&quot;').replace(/'/g,'&#039;');
    }
}

// Boot
let dashboard = null;
window.addEventListener('DOMContentLoaded', () => {
    dashboard = new DistriKvDashboard();
});
