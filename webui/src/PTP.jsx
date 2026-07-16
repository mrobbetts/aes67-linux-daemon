//
//  PTP.jsx
//
//  Copyright (c) 2019 2020 Andrea Bondavalli. All rights reserved.
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//

import React, {Component} from 'react';
import PropTypes from 'prop-types';
import {toast} from 'react-toastify';

import RestAPI from './Services';
import Loader from './Loader';


class PTPConfig extends Component {
  static propTypes = {
    domain: PropTypes.number.isRequired,
    dscp: PropTypes.number.isRequired,
  };

  constructor(props) {
    super(props);
    this.state = {
      domain: this.props.domain,
      dscp: this.props.dscp,
      domainErr: false,
    };
    this.onSubmit = this.onSubmit.bind(this);
    this.inputIsValid = this.inputIsValid.bind(this);
  }

  inputIsValid() {
    return !this.state.domainErr;
  }

  onSubmit(event) {
    event.preventDefault();
    RestAPI.setPTPConfig(this.state.domain, this.state.dscp)
      .then(response => toast.success('PTP config updated'));
  }

  render() {
    return (
     <div>
      <h3>Global</h3>
      <table><tbody>
        <tr>
          <th align="left"> <label>Type</label> </th>
          <th align="left"> <label>PTPv2</label> </th>
        </tr>
        <tr>
          <th align="left"> <label>DSCP</label> </th>
          <th align="left">
            <select value={this.state.dscp} onChange={e => this.setState({dscp: e.target.value})}>
              <option value="56">56 (CS7)</option>
              <option value="48">48 (CS6)</option>
              <option value="46">46 (EF)</option>
              <option value="36">36 (AF42)</option>
              <option value="34">34 (AF41)</option>
              <option value="0">0 (BE)</option>
            </select>
          </th>
        </tr>
        <tr>
          <th> <button disabled={this.inputIsValid() ? undefined : true} onClick={this.onSubmit}>Submit</button> </th>
        </tr>
      </tbody></table>
     </div>
    )
  }
}

/* W16 slice 5 — pure formatters for the GM's Announce properties. Well-known
 * values get their IEEE-1588 meaning; anything else shows the raw number. */
const fmtPpm = (ppb) => {
  const ppm = ppb / 1000;
  return (ppm >= 0 ? '+' : '') + ppm.toFixed(1) + ' ppm vs local';
};
const fmtClockClass = (c) => {
  const known = {6: 'primary reference (GNSS-sync)', 7: 'primary, holdover',
                 13: 'application-specific', 14: 'application, holdover',
                 52: 'degraded A', 58: 'degraded B',
                 248: 'default (free-running)', 255: 'slave-only'};
  return known[c] ? c + ' — ' + known[c] : String(c);
};
const fmtClockAccuracy = (a) => {
  const known = {0x20: '25 ns', 0x21: '100 ns', 0x22: '250 ns', 0x23: '1 µs',
                 0x24: '2.5 µs', 0x25: '10 µs', 0x26: '25 µs', 0x27: '100 µs',
                 0x28: '250 µs', 0x29: '1 ms', 0x2A: '2.5 ms', 0x2B: '10 ms',
                 0x2C: '25 ms', 0x2D: '100 ms', 0x2E: '250 ms', 0x2F: '1 s',
                 0x30: '10 s', 0x31: '> 10 s', 0xFE: 'unknown'};
  const hex = '0x' + a.toString(16).toUpperCase().padStart(2, '0');
  return known[a] ? 'within ' + known[a] + ' (' + hex + ')' : hex;
};
const fmtTimeSource = (t) => {
  const known = {0x10: 'atomic clock', 0x20: 'GNSS', 0x30: 'terrestrial radio',
                 0x40: 'PTP', 0x50: 'NTP', 0x60: 'hand-set', 0x90: 'other',
                 0xA0: 'internal oscillator'};
  const hex = '0x' + t.toString(16).toUpperCase().padStart(2, '0');
  return known[t] ? known[t] + ' (' + hex + ')' : hex;
};

class PTPDomainStatus extends Component {
  static propTypes = {
    info: PropTypes.object.isRequired,
  };

  render() {
    const d = this.props.info;
    // W16 slice 5: the header is the DOMAIN's clock-source health — the
    // composite clock_state (an untrackable GM shows "saturated" in red even
    // though the PTP servo itself tracks it fine; the layered truth is in the
    // "PTP servo" row below). Legacy status is the fallback for old daemons.
    const state = d.clock_state || d.status;
    const color = state === 'locked' ? '#2a0'
                : (state === 'acquiring' || state === 'locking') ? '#d90' : '#c00';
    // without a PTP signal there is no reference clock at all — neither the
    // grandmaster identity nor any of its properties is meaningful; blank all.
    // (saturated is different: the GM is present and its properties are exactly
    // what you want to see.)
    const meaningful = state !== 'no-signal' && state !== 'unlocked';
    const val = (v) => meaningful ? v : '—';
    const row = (label, value, tip) => (
      <tr>
        <th align="left"> <label title={tip}>{label}</label> </th>
        <th align="left"> <input size="30" value={value} disabled/> </th>
      </tr>);
    return (
     <div style={{marginBottom: '1em'}}>
      <h3>Domain {d.domain}&nbsp;&nbsp;
        <span style={{color: color}}>&#9679;</span>&nbsp;
        <span style={{color: color}}>{state}</span>
      </h3>
      <table><tbody>
        {row('PTP servo', meaningful ? 'locked' : 'unlocked',
             'the PTP time servo itself — it can track a GM (offset estimation) even when the media servo cannot (rate beyond steering range = saturated)')}
        {row('GMID', val(d.gmid))}
        {row('Clock jitter', val(parseInt(d.jitter, 10)))}
        {/* W16 slice 5: the GM's Announce properties + our measured rate offset.
          * Displayed, never gated on — a poor clockClass is information, not a
          * veto (the freewheel taught us the announce can say one thing and the
          * rate another; the ppm row is what WE measure). */}
        {row('GM rate offset', val(fmtPpm(d.gm_rate_ppb || 0)),
             'measured by our servo: the GM’s rate vs our local reference — beyond ~±370 ppm the media servo saturates (untrackable)')}
        {row('Clock class', val(fmtClockClass(d.gm_clock_class || 0)),
             'announced clockClass — the GM’s own claim about its reference')}
        {row('Clock accuracy', val(fmtClockAccuracy(d.gm_clock_accuracy || 0)),
             'announced clockAccuracy')}
        {row('Log variance', val(d.gm_log_variance),
             'announced offsetScaledLogVariance (0xFFFF = not computed)')}
        {row('Priority 1 / 2', val((d.gm_priority1 ?? '—') + ' / ' + (d.gm_priority2 ?? '—')),
             'BMCA priorities from the announce')}
        {row('Steps removed', val(d.gm_steps_removed),
             'boundary-clock hops between the GM and us')}
        {row('Time source', val(fmtTimeSource(d.gm_time_source || 0)),
             'announced timeSource')}
      </tbody></table>
     </div>
    )
  }
}

class PTP extends Component {
  constructor(props) {
    super(props);
    this.state = {
      domain: 0,
      domainErr: false,
      dscp: 0,
      domains: [],
      isConfigLoading: false,
      isStatusLoading: false,
    };
  }

  fetchDomains() {
    this.setState({isStatusLoading: true});
    RestAPI.getPTPDomains()
      .then(response => response.json())
      .then(
        data => this.setState({
           domains: data.domains || [],
           isStatusLoading: false
        }))
      .catch(err => this.setState({isStatusLoading: false}));
  }

  fetchConfig() {
    this.setState({isConfigLoading: true});
    RestAPI.getPTPConfig()
      .then(response => response.json())
      .then(
        data => this.setState({
          domain: parseInt(data.domain, 10),
          dscp: parseInt(data.dscp, 10),
          isConfigLoading: false
        }))
      .catch(err => this.setState({isConfigLoading: false}));
  }

  componentDidMount() {
    this.fetchConfig();
    this.fetchDomains();
    this.interval = setInterval(() => { this.fetchDomains() }, 3000)
  }

  componentWillUnmount() {
    clearInterval(this.interval);
  }

  render() {
    return (
      <div className='ptp'>
        { this.state.isConfigLoading ? <Loader/> :
           <PTPConfig domain={this.state.domain} dscp={this.state.dscp}/> }
        <br/>
        <h2>Clocks</h2>
        { this.state.domains.length === 0 ?
            <p>No active PTP domains &mdash; they appear here as cards use them.</p> :
            this.state.domains.map(d =>
              <PTPDomainStatus key={d.domain} info={d}/>) }
      </div>
    )
  }
}

export default PTP;
