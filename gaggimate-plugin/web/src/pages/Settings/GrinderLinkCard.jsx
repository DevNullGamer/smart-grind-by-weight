import { useContext, useEffect, useRef, useState } from 'preact/hooks';
import { Trans, useLingui } from '@lingui/react/macro';
import { ApiServiceContext } from '../../services/ApiService.js';

// GrinderLink card (prototype): talks to the GLinkPlugin over the WebSocket, req:grinderlink:* / evt:grinderlink:*.
// The plugin owns the Bluetooth link; this card only sends requests and shows the state it publishes.

const PROFILE_DOSE = 3; // the one profile the grinder lets Bluetooth write
const KEEPALIVE_MS = 20000; // the plugin drops an unused link after 60 s
const STATUS_TIMEOUT_MS = 3000; // no answer means the plugin is not running (needs a restart)
const ACTION_TIMEOUT_MS = 15000; // a connect can take a few seconds

function withTimeout(promise, ms) {
  let timer;
  const timeout = new Promise((_, reject) => {
    timer = setTimeout(() => reject(new Error('timeout')), ms);
  });
  return Promise.race([promise, timeout]).finally(() => clearTimeout(timer));
}

const isGrinding = state => state === 'GRINDING' || state === 'STARTING';

export function GrinderLinkCard({ formData, onChange }) {
  const { t } = useLingui();
  const apiService = useContext(ApiServiceContext);
  const [status, setStatus] = useState(null); // last status from the plugin, null = plugin not answering
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState('');
  const [dose, setDose] = useState('');
  const [size, setSize] = useState('');
  const [unit, setUnit] = useState('');
  const touched = useRef(false); // the user typed a value: stop overwriting the fields from the grinder
  const enabled = !!formData.grinderLinkActive;

  const ask = (tp, body = {}, timeoutMs = ACTION_TIMEOUT_MS) =>
    withTimeout(apiService.request({ tp, ...body }), timeoutMs).then(res => {
      if (res.ok === false) {
        const text = [res.error, res.message].filter(Boolean).join(': ');
        throw new Error(text || t`Request failed`);
      }
      if (res.link !== undefined) setStatus(res);
      return res;
    });

  const run = async (fn, keepMessage = false) => {
    setBusy(true);
    if (!keepMessage) setMessage('');
    try {
      await fn();
    } catch (e) {
      setMessage(e.message);
    } finally {
      setBusy(false);
    }
  };

  // Subscribe to the plugin's state events and read the first status. No answer: the plugin is not running.
  useEffect(() => {
    if (!enabled || !apiService) return undefined;
    let alive = true;
    const listener = apiService.on('evt:grinderlink:state', msg => alive && setStatus(msg));
    ask('req:grinderlink:status', {}, STATUS_TIMEOUT_MS).catch(() => alive && setStatus(null));
    return () => {
      alive = false;
      apiService.off('evt:grinderlink:state', listener);
    };
  }, [enabled, apiService]);

  // Keep the link open while the card shows a linked grinder; never starts anything.
  const linked = status?.link === 'ready' || status?.link === 'linked';
  useEffect(() => {
    if (!linked) return undefined;
    const timer = setInterval(() => ask('req:grinderlink:connect', { keepalive: true }, ACTION_TIMEOUT_MS).catch(() => {}), KEEPALIVE_MS);
    return () => clearInterval(timer);
  }, [linked]);

  // Fill the fields from the grinder's Profile Dose until the user edits them.
  const stored = status?.profiles?.find(p => p.index === PROFILE_DOSE);
  useEffect(() => {
    if (!stored || touched.current) return;
    setDose(String(stored.dose));
    setSize(stored.grindSize === undefined ? '' : String(stored.grindSize));
    setUnit(stored.grindUnit || '');
  }, [stored?.dose, stored?.grindSize, stored?.grindUnit]);

  const edit = setter => e => {
    touched.current = true;
    setter(e.currentTarget.value);
  };

  const doseNumber = parseFloat(dose);
  const doseValid = Number.isFinite(doseNumber) && doseNumber >= 5 && doseNumber <= 60;
  const sizeValid = size === '' || (Number.isFinite(parseFloat(size)) && parseFloat(size) >= 0 && parseFloat(size) <= 999.9);
  const unitValid = unit.length <= 6 && (size !== '' || unit === '');

  const profileBody = () => {
    const body = { profile: PROFILE_DOSE, dose: doseNumber };
    if (size === '') {
      body.grindSize = null; // clears the size and its unit
    } else {
      body.grindSize = parseFloat(size);
      body.grindUnit = unit === '' ? null : unit;
    }
    return body;
  };

  const select = found =>
    run(() =>
      ask('req:grinderlink:select', { address: found.address, addrType: found.addrType, name: found.name }),
    );
  const send = () => run(() => ask('req:grinderlink:profile:update', profileBody()));
  // The dose goes first, then the start with the same dose as expectDose (the plugin always sends it).
  const start = () =>
    run(async () => {
      await ask('req:grinderlink:profile:update', profileBody());
      await ask('req:grinderlink:start', { profile: PROFILE_DOSE, dose: doseNumber });
    });
  const stop = () => run(() => ask('req:grinderlink:stop'));

  const period = status?.period;
  const linkText = {
    idle: t`Not connected`,
    scanning: t`Scanning...`,
    connecting: t`Connecting...`,
    linked: t`Waiting for the grinder`,
    ready: t`Connected`,
  }[status?.link || 'idle'];
  const outcome = status?.startOutcome;
  const result = status?.lastResult;
  const grinding = isGrinding(period?.devState);

  return (
    <div className='bg-base-200 rounded-lg p-4'>
      <div className='flex items-center justify-between'>
        <span className='text-xl font-medium'>
          <Trans>GrinderLink (Smart Grind By Weight)</Trans>
        </span>
        <input
          id='grinderLinkActive'
          name='grinderLinkActive'
          value='grinderLinkActive'
          type='checkbox'
          className='toggle toggle-primary'
          checked={enabled}
          onChange={onChange('grinderLinkActive')}
          aria-label={t`Enable GrinderLink`}
        />
      </div>
      {enabled && (
        <div className='border-base-300 mt-4 space-y-4 border-t pt-4'>
          <p className='text-sm opacity-70'>
            <Trans>
              Lets GaggiMate talk to a Smart Grind By Weight grinder over Bluetooth: set its dose,
              start and stop a grind and read the result. Save and restart to apply the switch.
            </Trans>
          </p>
          {status === null ? (
            <p className='text-sm'>
              <Trans>GrinderLink is not running yet. Save and restart GaggiMate to start it.</Trans>
            </p>
          ) : (
            <>
              <div className='form-control'>
                <span className='mb-2 block text-sm font-medium'>
                  <Trans>Grinder</Trans>
                </span>
                <div className='flex flex-wrap items-center gap-2'>
                  <code className='bg-base-300 rounded px-2 py-1'>
                    {status.name || status.address || t`None selected`}
                  </code>
                  <span className='text-sm opacity-70'>{linkText}</span>
                </div>
                {status.error && <div className='text-error mt-1 text-sm'>{status.error}</div>}
              </div>
              <div className='flex flex-wrap gap-2'>
                <button
                  type='button'
                  className='btn btn-primary btn-sm'
                  disabled={busy || status.link === 'scanning' || linked}
                  onClick={() => run(() => ask('req:grinderlink:scan'))}
                >
                  {status.link === 'scanning' ? t`Scanning...` : t`Scan`}
                </button>
                {linked ? (
                  <button
                    type='button'
                    className='btn btn-sm'
                    disabled={busy}
                    onClick={() => run(() => ask('req:grinderlink:disconnect'))}
                  >
                    {t`Disconnect`}
                  </button>
                ) : (
                  <button
                    type='button'
                    className='btn btn-sm'
                    disabled={busy || !status.address}
                    onClick={() => run(() => ask('req:grinderlink:connect'))}
                  >
                    {t`Connect`}
                  </button>
                )}
                <button
                  type='button'
                  className='btn btn-ghost btn-sm'
                  disabled={busy || !status.address}
                  onClick={() => run(() => ask('req:grinderlink:forget'))}
                >
                  {t`Forget`}
                </button>
              </div>
              {status.found?.length > 0 && (
                <ul className='space-y-1'>
                  {status.found.map(f => (
                    <li key={f.address} className='flex items-center justify-between gap-2'>
                      <span className='text-sm'>
                        {f.name} <span className='opacity-60'>{f.address}</span>
                      </span>
                      <button
                        type='button'
                        className='btn btn-sm'
                        disabled={busy}
                        onClick={() => select(f)}
                      >
                        {t`Select`}
                      </button>
                    </li>
                  ))}
                </ul>
              )}
              {status.link === 'idle' && status.found?.length === 0 && !status.address && (
                <p className='text-sm opacity-70'>
                  <Trans>No grinder found yet. Wake the grinder and scan again.</Trans>
                </p>
              )}

              {status.link === 'ready' && (
                <>
                  <div className='grid grid-cols-3 gap-4'>
                    <div className='form-control'>
                      <label htmlFor='glDose' className='mb-1 block text-sm font-medium'>
                        {t`Dose (g)`}
                      </label>
                      <input
                        id='glDose'
                        type='number'
                        min='5'
                        max='60'
                        step='0.1'
                        className='input input-bordered w-full'
                        value={dose}
                        onInput={edit(setDose)}
                      />
                    </div>
                    <div className='form-control'>
                      <label htmlFor='glSize' className='mb-1 block text-sm font-medium'>
                        {t`Grind size`}
                      </label>
                      <input
                        id='glSize'
                        type='number'
                        min='0'
                        max='999.9'
                        step='0.1'
                        className='input input-bordered w-full'
                        value={size}
                        onInput={edit(setSize)}
                      />
                    </div>
                    <div className='form-control'>
                      <label htmlFor='glUnit' className='mb-1 block text-sm font-medium'>
                        {t`Unit`}
                      </label>
                      <input
                        id='glUnit'
                        type='text'
                        maxLength={6}
                        className='input input-bordered w-full'
                        value={unit}
                        onInput={edit(setUnit)}
                      />
                    </div>
                  </div>
                  <div className='flex flex-wrap gap-2'>
                    <button
                      type='button'
                      className='btn btn-sm'
                      disabled={busy || !doseValid || !sizeValid || !unitValid}
                      onClick={send}
                    >
                      {t`Send to grinder`}
                    </button>
                    <button
                      type='button'
                      className='btn btn-primary btn-sm'
                      disabled={busy || grinding || !doseValid || !sizeValid || !unitValid}
                      onClick={start}
                    >
                      {t`Start grind`}
                    </button>
                    <button type='button' className='btn btn-error btn-sm' onClick={stop}>
                      {t`Stop`}
                    </button>
                  </div>
                </>
              )}

              {period && (
                <div className='text-sm'>
                  <span className='opacity-70'>{t`Grinder state`}: </span>
                  {period.devState}
                  {period.blocked ? ` (${period.blocked})` : ''}
                  {period.weight !== null && period.weight !== undefined && (
                    <>
                      {' '}
                      <span className='opacity-70'>{t`Weight`}: </span>
                      {period.weight.toFixed(1)} g
                    </>
                  )}
                </div>
              )}
              {outcome === 'not_started' && (
                <div className='text-warning text-sm'>{t`The grinder did not take the start.`}</div>
              )}
              {(outcome === 'not_seen' || outcome === 'unknown') && (
                <div className='text-warning text-sm'>
                  {t`No start was seen. Check the grinder before you start again.`}
                </div>
              )}
              {result && (
                <div className='text-sm'>
                  <span className='opacity-70'>{t`Last result`}: </span>
                  {result.code}, {result.finalWeight.toFixed(1)} g / {result.target.toFixed(1)} g
                </div>
              )}
              {message && <div className='text-error text-sm'>{message}</div>}
            </>
          )}
        </div>
      )}
    </div>
  );
}
