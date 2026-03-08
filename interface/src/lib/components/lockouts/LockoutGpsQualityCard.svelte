<script>
	import CardSectionHead from '$lib/components/CardSectionHead.svelte';
	import { formatHdop, GPS_MIN_SATELLITES } from '$lib/utils/lockout';

	let { gpsStatus, lockoutConfig } = $props();
</script>

<div class="surface-card">
	<div class="card-body gap-3">
		<CardSectionHead
			title="GPS Quality"
			subtitle="Live GPS fix quality determines whether lockout evaluation and learning are active."
		/>
		<div class="surface-stats">
			<div class="stat py-3 px-4">
				<div class="stat-title">Satellites</div>
				<div
					class="stat-value text-base"
					class:text-error={typeof gpsStatus?.satellites === 'number' && gpsStatus.satellites < GPS_MIN_SATELLITES}
					class:text-success={typeof gpsStatus?.satellites === 'number' && gpsStatus.satellites >= GPS_MIN_SATELLITES}
				>
					{typeof gpsStatus?.satellites === 'number' ? gpsStatus.satellites : '—'}
				</div>
				<div class="stat-desc">min {GPS_MIN_SATELLITES} required</div>
			</div>
			<div class="stat py-3 px-4">
				<div class="stat-title">HDOP</div>
				<div
					class="stat-value text-base"
					class:text-error={typeof gpsStatus?.hdop === 'number' && gpsStatus.hdop > lockoutConfig.maxHdopX10 / 10}
					class:text-success={typeof gpsStatus?.hdop === 'number' && gpsStatus.hdop <= lockoutConfig.maxHdopX10 / 10}
				>
					{formatHdop(gpsStatus?.hdop)}
				</div>
				<div class="stat-desc">max {(lockoutConfig.maxHdopX10 / 10).toFixed(1)} allowed</div>
			</div>
			<div class="stat py-3 px-4">
				<div class="stat-title">Speed</div>
				<div
					class="stat-value text-base"
					class:text-warning={typeof gpsStatus?.speedMph === 'number' && lockoutConfig.minLearnerSpeedMph > 0 && gpsStatus.speedMph < lockoutConfig.minLearnerSpeedMph}
				>
					{typeof gpsStatus?.speedMph === 'number' ? `${Math.round(gpsStatus.speedMph)} mph` : '—'}
				</div>
				<div class="stat-desc">
					{lockoutConfig.minLearnerSpeedMph > 0
						? `min ${lockoutConfig.minLearnerSpeedMph} mph for learning`
						: 'no speed gate'}
				</div>
			</div>
			<div class="stat py-3 px-4">
				<div class="stat-title">Fix</div>
				<div class="stat-value text-base" class:text-success={gpsStatus?.hasFix} class:text-error={!gpsStatus?.hasFix}>
					{gpsStatus?.hasFix ? 'Yes' : 'No'}
				</div>
				<div class="stat-desc">{gpsStatus?.locationValid ? 'location valid' : 'no position'}</div>
			</div>
		</div>
	</div>
</div>
