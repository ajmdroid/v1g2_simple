<script>
	import '../app.css';
	let { children } = $props();
</script>

<div class="min-h-screen bg-base-100">
	<!-- Navigation -->
	<nav class="navbar bg-base-200 shadow-lg" aria-label="Main navigation">
		<div class="navbar-start">
			<div class="dropdown">
			<button type="button" aria-label="Open navigation menu" aria-expanded="false" class="btn btn-ghost lg:hidden">
				<svg xmlns="http://www.w3.org/2000/svg" class="h-5 w-5" fill="none" viewBox="0 0 24 24" stroke="currentColor" aria-hidden="true">
					<path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M4 6h16M4 12h8m-8 6h16" />
				</svg>
			</button>
			<ul class="menu menu-sm dropdown-content mt-3 z-[1] p-2 shadow bg-base-200 rounded-box w-52" role="menu">
					<li><a href="/">Dashboard</a></li>
					<li><a href="/autopush">Auto-Push</a></li>
					<li><a href="/profiles">V1 Profiles</a></li>
					<li><a href="/devices">Saved V1s</a></li>
					<li><a href="/colors">Colors</a></li>
					<li><a href="/audio">Audio</a></li>
					<li><a href="/integrations">Integrations</a></li>
					<li><a href="/settings">Settings</a></li>
				</ul>
			</div>
			<a href="/" class="btn btn-ghost text-xl text-primary">V1 Gen2</a>
		</div>
		<div class="navbar-center hidden lg:flex">
			<ul class="menu menu-horizontal px-1">
				<li><a href="/" class="hover:text-primary">Dashboard</a></li>
				<li><a href="/autopush" class="hover:text-primary">Auto-Push</a></li>
				<li><a href="/profiles" class="hover:text-primary">Profiles</a></li>
				<li><a href="/colors" class="hover:text-primary">Colors</a></li>
				<li><a href="/audio" class="hover:text-primary">Audio</a></li>
				<li><a href="/integrations" class="hover:text-primary">Integrations</a></li>
				<li><a href="/settings" class="hover:text-primary">Settings</a></li>
			</ul>
		</div>
		<div class="navbar-end">
			<div class="badge badge-outline badge-sm" id="connection-status">
				<span class="loading loading-dots loading-xs" aria-label="Checking connection"></span>
			</div>
		</div>
	</nav>

	<!-- Main Content -->
	<main class="container mx-auto p-4 max-w-4xl">
		{@render children()}
	</main>

	<!-- Footer -->
	<footer class="footer footer-center p-4 bg-base-200 text-base-content mt-8">
		<aside>
			<p>V1 Gen2 Display • <a href="https://github.com/ajmdroid/v1g2_simple" class="link link-primary">GitHub</a></p>
		</aside>
	</footer>
</div>
