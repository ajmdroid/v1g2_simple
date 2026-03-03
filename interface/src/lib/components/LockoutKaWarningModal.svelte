<script>
	/** @type {{ show: boolean, oncancel: () => void, onconfirm: () => void }} */
	let { show, oncancel, onconfirm } = $props();
</script>

{#if show}
	<div class="modal modal-open">
		<div class="modal-box surface-modal">
			<h3 class="font-bold text-lg">Ka Lockout Learning Warning</h3>
			<p class="py-3 copy-warning">
				High risk: enabling Ka lockout learning can suppress real Ka threats.
			</p>
			<p class="copy-caption-soft">
				Ka lockouts can hide real threats. Keep this off unless you fully understand the risk.
			</p>
			<div class="modal-action">
				<button class="btn btn-outline btn-sm" onclick={oncancel}>
					Cancel (recommended)
				</button>
				<button class="btn btn-warning btn-sm" onclick={onconfirm}>
					Enable anyway (high risk)
				</button>
			</div>
		</div>
	</div>
{/if}
