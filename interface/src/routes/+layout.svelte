<script>
	import '../app.css';
	import { onMount } from 'svelte';
	import BrandMark from '$lib/components/BrandMark.svelte';
	
	let { children } = $props();
	let showPasswordWarning = $state(false);
	let warningDismissed = $state(false);
	const DEFAULT_PASSWORD_CACHE_KEY = 'v1simple:isDefaultPassword';
	const DEFAULT_PASSWORD_DISMISSED_KEY = 'passwordWarningDismissed';
	const TIME_SYNC_CACHE_KEY = 'v1simple:lastTimeSyncMs';
	const TIME_SYNC_MIN_INTERVAL_MS = 10 * 60 * 1000;
	const navLinks = [
		{ href: '/', label: 'Dashboard' },
		{ href: '/autopush', label: 'Auto-Push' },
		{ href: '/profiles', label: 'Profiles' },
		{ href: '/devices', label: 'Devices' },
		{ href: '/colors', label: 'Colors' },
		{ href: '/audio', label: 'Audio' },
		{ href: '/lockouts', label: 'Lockouts' },
		{ href: '/cameras', label: 'Cameras' },
		{ href: '/integrations', label: 'Integrations' },
		{ href: '/settings', label: 'Settings' }
	];
	const advancedLinks = [{ href: '/dev', label: 'Development' }];

	function runWhenIdle(callback, fallbackDelayMs = 250) {
		if (typeof window !== 'undefined' && 'requestIdleCallback' in window) {
			window.requestIdleCallback(callback, { timeout: 1500 });
			return;
		}
		setTimeout(callback, fallbackDelayMs);
	}

	function scheduleClientTimeSync() {
		const now = Date.now();
		const lastSyncMs = Number(sessionStorage.getItem(TIME_SYNC_CACHE_KEY) || '0');
		if (Number.isFinite(lastSyncMs) && lastSyncMs > 0 && now - lastSyncMs < TIME_SYNC_MIN_INTERVAL_MS) {
			return;
		}
		sessionStorage.setItem(TIME_SYNC_CACHE_KEY, String(now));
		runWhenIdle(() => {
			fetch('/api/time/set', {
				method: 'POST',
				headers: { 'Content-Type': 'application/json' },
				body: JSON.stringify({
					unixMs: Date.now(),
					tzOffsetMin: new Date().getTimezoneOffset() * -1,
					source: 'client'
				})
			}).catch(() => {});
		}, 300);
	}
	
	// Check if using default password on mount
	onMount(() => {
		scheduleClientTimeSync();

		if (sessionStorage.getItem(DEFAULT_PASSWORD_DISMISSED_KEY)) {
			warningDismissed = true;
			return;
		}

		const cachedDefaultPassword = sessionStorage.getItem(DEFAULT_PASSWORD_CACHE_KEY);
		if (cachedDefaultPassword !== null) {
			showPasswordWarning = cachedDefaultPassword === '1';
			return;
		}

		runWhenIdle(async () => {
			try {
				const res = await fetch('/api/settings');
				if (!res.ok) return;
				const data = await res.json();
				const isDefaultPassword = data.isDefaultPassword === true;
				showPasswordWarning = isDefaultPassword;
				sessionStorage.setItem(DEFAULT_PASSWORD_CACHE_KEY, isDefaultPassword ? '1' : '0');
			} catch (e) {
				// Don't show warning on error.
			}
		}, 600);
	});
	
	function dismissWarning() {
		warningDismissed = true;
		sessionStorage.setItem(DEFAULT_PASSWORD_DISMISSED_KEY, 'true');
	}
</script>

<div class="min-h-screen bg-base-100">
	<!-- Security Warning Banner -->
	{#if showPasswordWarning && !warningDismissed}
		<div class="surface-alert alert-warning banner" role="alert">
			<svg xmlns="http://www.w3.org/2000/svg" class="stroke-current shrink-0 h-6 w-6" fill="none" viewBox="0 0 24 24">
				<path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 9v2m0 4h.01m-6.938 4h13.856c1.54 0 2.502-1.667 1.732-3L13.732 4c-.77-1.333-2.694-1.333-3.464 0L3.34 16c-.77 1.333.192 3 1.732 3z" />
			</svg>
			<div>
				<h3 class="font-bold">Default Password Detected</h3>
				<div class="text-xs">Change your WiFi password in <a href="/settings" class="link link-primary font-semibold">Settings</a> to secure your device.</div>
			</div>
			<button class="btn btn-sm btn-ghost" onclick={dismissWarning} aria-label="Dismiss warning">✕</button>
		</div>
	{/if}

	<!-- Navigation -->
	<nav class="navbar surface-chrome border-b" aria-label="Main navigation">
		<div class="navbar-start">
			<div class="dropdown">
			<button type="button" aria-label="Open navigation menu" aria-expanded="false" class="btn btn-ghost lg:hidden">
				<svg xmlns="http://www.w3.org/2000/svg" class="h-5 w-5" fill="none" viewBox="0 0 24 24" stroke="currentColor" aria-hidden="true">
					<path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M4 6h16M4 12h8m-8 6h16" />
				</svg>
			</button>
			<ul class="menu menu-sm dropdown-content mt-3 z-[1] w-52 surface-menu" role="menu">
					{#each navLinks as link}
						<li><a href={link.href}>{link.label}</a></li>
					{/each}
					<li class="menu-title"><span>Advanced</span></li>
					{#each advancedLinks as link}
						<li><a href={link.href} class="text-warning">{link.label}</a></li>
					{/each}
				</ul>
			</div>
			<a href="/" class="btn btn-ghost h-auto min-h-0 px-2 py-1">
				<BrandMark compact />
			</a>
		</div>
		<div class="navbar-center hidden lg:flex">
			<ul class="menu menu-horizontal px-1">
				{#each navLinks as link}
					<li><a href={link.href} class="hover:text-primary">{link.label}</a></li>
				{/each}
				{#each advancedLinks as link}
					<li><a href={link.href} class="hover:text-warning text-warning">{link.label}</a></li>
				{/each}
			</ul>
		</div>
		<div class="navbar-end">
			<div class="badge badge-outline badge-sm" id="connection-status">
				<span class="loading loading-dots loading-xs" aria-label="Checking connection"></span>
			</div>
		</div>
	</nav>

	<!-- Main Content -->
	<main class="container mx-auto p-4 max-w-6xl">
		{@render children()}
	</main>

	<!-- Footer -->
	<footer class="footer footer-center p-4 surface-chrome border-t text-base-content mt-8">
		<aside>
			<div class="flex flex-wrap items-center justify-center gap-2">
				<BrandMark compact />
				<span class="text-sm text-base-content/60">•</span>
				<a href="https://github.com/ajmdroid/v1g2_simple" class="link link-primary">GitHub</a>
			</div>
		</aside>
	</footer>
</div>
