//
//  Sinks.jsx
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

import RestAPI from './Services';
import Loader from './Loader';
import SinkEdit from './SinkEdit';
import SinkRemove from './SinkRemove';

class SinkEntry extends Component {
  static propTypes = {
    id: PropTypes.number.isRequired,
    name: PropTypes.string.isRequired,
    channels: PropTypes.number.isRequired,
    attached: PropTypes.bool.isRequired,
    detachReason: PropTypes.string,
    onEditClick: PropTypes.func.isRequired,
    onTrashClick: PropTypes.func.isRequired
  };

  constructor(props) {
    super(props);
    this.state = {
      min_time: 'n/a',
      flags: 'n/a',
      flagsColor: '#888',
      errors: 'n/a',
      offset: null,        // samples (kernel receive margin)
      rate: 0,             // the pcm's live rate, for ms/ppm conversion
      driftPpm: null       // sender clock vs ours, from the offset trend
    };
    // #32: drift baseline — {t (ms), offset (samples)}; reset on jumps.
    this.driftBase = null;
  }

  handleEditClick = () => {
    this.props.onEditClick(this.props.id);
  };

  handleTrashClick = () => {
    this.props.onTrashClick(this.props.id);
  };

  // #32: poll the sink status live (it used to be fetched once at mount).
  fetchStatus = () => {
    /* a detached sink (driver refused the last (re)add — standing intent,
       daemon retrying) has no kernel stream to poll */
    if (!this.props.attached) {
      return;
    }
    RestAPI.getSinkStatus(this.props.id)
      .then(response => response.json())
      .then(function(status) {
        let errors = '';
        if (status.sink_flags.rtp_seq_id_error)
          errors += 'SEQID';
        if (status.sink_flags.rtp_ssrc_error)
          errors += (errors ? ',' : '') + 'SSRC';
        if (status.sink_flags.rtp_payload_type_error)
          errors += (errors ? ',' : '') + 'payload type';
        if (status.sink_flags.rtp_sac_error)
          errors += (errors ? ',' : '') + 'SAC';
        // (fixed: these keys had stray underscores, so mute states never showed)
        let flags = '';
        let color = '#888';
        if (status.sink_flags.receiving_rtp_packet) {
          flags = 'receiving';
          color = '#2a0';
        }
        if (status.sink_flags.some_muted)
          flags += (flags ? ', ' : '') + 'some muted';
        if (status.sink_flags.all_muted || status.sink_flags.muted) {
          flags += (flags ? ', ' : '') + 'muted';
          color = '#c00';
        }
        if (!flags)
          flags = 'idle';

        // #32: sender-drift estimate from the receive-margin trend. Sender and
        // sink share a PTP GM, so the margin should be FLAT; a steady slope is
        // the sender free-wheeling (the VAD failure class). Slope measured
        // against a retained baseline; re-baselined on any jump (re-add /
        // re-anchor / start) so one discontinuity doesn't fake a huge drift.
        const rate = status.sink_sample_rate || 0;
        const receiving = status.sink_flags.receiving_rtp_packet;
        let offset = receiving ? status.sink_receive_offset : null;
        // Guard (bench 2026-07-19): an exact-0 margin while receiving is the
        // legacy kernel's "status tick saw no packet" artifact (per-packet
        // cadence aliasing the tick rate) — never real audio (0 = mute floor
        // crossed long before). Hold the last value so a glitch sample can
        // neither seed nor reset the drift baseline (+398.8 ppm, once).
        if (offset === 0 && receiving) {
          offset = this.state.offset;
        }
        let driftPpm = this.state.driftPpm;
        if (offset === null || !rate) {
          this.driftBase = null;
          driftPpm = null;
        } else {
          const now = Date.now();
          const jumped = this.driftBase &&
            Math.abs(offset - this.driftBase.offset) > rate / 2;  // >0.5 s step
          if (!this.driftBase || jumped) {
            this.driftBase = { t: now, offset: offset };
            driftPpm = null;
          } else {
            const dt = (now - this.driftBase.t) / 1000;
            // 30 s minimum: at 15 s a few samples of arrival jitter reads as
            // ±8 ppm and flashed false ambers/reds on healthy sinks (bench
            // 2026-07-19, "-11.1 ppm" on a locked sender). Noise at 30 s is
            // under the 3 ppm amber threshold; precision keeps improving as
            // the baseline ages.
            if (dt >= 30) {
              driftPpm = ((offset - this.driftBase.offset) / rate / dt) * 1e6;
            }
          }
        }

        this.setState({
          min_time: status.sink_min_time + ' ms',
          flags: flags,
          flagsColor: color,
          errors: errors ? errors : 'none',
          offset: offset,
          rate: rate,
          driftPpm: driftPpm
        });
      }.bind(this));
  };

  componentDidMount() {
    this.fetchStatus();
    this.statusTimer = setInterval(this.fetchStatus, 3000);
  }

  componentWillUnmount() {
    clearInterval(this.statusTimer);
  }

  render() {
    // #32: receive margin as samples + ms; drift coloured by how locked the
    // sender is to our clock (shared GM => ~0 ppm; a freewheeler shows tens
    // to thousands of ppm and WILL eventually garble).
    const offsetText = this.state.offset !== null && this.state.rate
      ? this.state.offset + ' smp (' +
        (this.state.offset / this.state.rate * 1000).toFixed(1) + ' ms)'
      : '—';
    const d = this.state.driftPpm;
    const driftText = d === null ? '—'
      : (d >= 0 ? '+' : '') + d.toFixed(1) + ' ppm';
    const driftColor = d === null ? '#888'
      : Math.abs(d) > 10 ? '#c00'
      : Math.abs(d) > 3 ? '#d90' : '#2a0';
    const driftTip = d === null
      ? 'sender clock vs ours — measuring (needs ~30 s of samples)'
      : 'sender clock vs ours, from the receive-margin trend: ~0 = locked to '
        + 'the same GM; a sustained offset means the sender is free-wheeling '
        + 'and this sink will eventually garble';
    return (
      <tr className='tr-stream'>
        <td> <label>{this.props.id}</label> </td>
        <td> <label>{this.props.name}</label> </td>
        <td align='center'> <label>{this.props.channels}</label> </td>
        <td align='center'>
          {this.props.attached
            ? <label style={{color: this.state.flagsColor}}>{this.state.flags}</label>
            : <label style={{color: '#c00'}}
                title={this.props.detachReason}>
                detached — {this.props.detachReason || 'driver refused; retrying'}
              </label>}
        </td>
        <td align='center'>
          <label style={{color: this.state.errors === 'none' ? '#888' : '#c00'}}>
            {this.state.errors}
          </label>
        </td>
        <td align='center'> <label>{this.state.min_time}</label> </td>
        <td align='center'> <label title='receive margin: received timestamps vs local playout'>{offsetText}</label> </td>
        <td align='center'>
          <label title={driftTip} style={{color: driftColor}}>{driftText}</label>
        </td>
        <td> <span className='pointer-area' onClick={this.handleEditClick}> <img width='20' height='20' src='/edit.png' alt=''/> </span> </td>
        <td> <span className='pointer-area' onClick={this.handleTrashClick}> <img width='20' height='20' src='/trash.png' alt=''/> </span> </td>
      </tr>
    );
  }
}


class SinkList extends Component {
  static propTypes = {
    onAddClick: PropTypes.func.isRequired,
    onReloadClick: PropTypes.func.isRequired
  };

  handleAddClick = () => {
    this.props.onAddClick();
  };

  handleReloadClick = () => {
    this.props.onReloadClick();
  };

  render() {
    return (
      <div id='sinks-table'>
        <table className="table-stream"><tbody>
          {this.props.sinks.length > 0 ?
            <tr className='tr-stream'>
              <th>ID</th>
              <th>Name</th>
              <th>Channels</th>
              <th>Status</th>
              <th>Errors</th>
              <th>Min. arrival time</th>
              <th>Receive margin</th>
              <th>Sender drift</th>
            </tr>
          : <tr>
             <th>No sinks configured</th>
            </tr> }
          {this.props.sinks}
        </tbody></table>
         &nbsp;
        <span className='pointer-area' onClick={this.handleReloadClick}> <img width='30' height='30' src='/reload.png' alt=''/> </span>
         &nbsp;&nbsp;
        {this.props.sinks.length < 64 ?
	  <span className='pointer-area' onClick={this.handleAddClick}> <img width='30' height='30' src='/plus.png' alt=''/> </span>
          : undefined}
      </div>
    );
  }
}

class Sinks extends Component {
  constructor(props) {
    super(props);
    this.state = {
      sinks: [],
      sink: {},
      isLoading: false,
      isEdit: false,
      editIsOpen: false,
      removeIsOpen: false,
      editTitle: ''
    };
    this.onEditClick = this.onEditClick.bind(this);
    this.onTrashClick = this.onTrashClick.bind(this);
    this.onAddClick = this.onAddClick.bind(this);
    this.onReloadClick = this.onReloadClick.bind(this);
    this.openEdit = this.openEdit.bind(this);
    this.closeEdit = this.closeEdit.bind(this);
    this.applyEdit = this.applyEdit.bind(this);
    this.fetchSinks = this.fetchSinks.bind(this);
  }

  fetchSinks() {
    this.setState({isLoading: true});
    RestAPI.getSinks()
      .then(response => response.json())
      .then(
        data => this.setState( { sinks: data.sinks, isLoading: false }))
      .catch(err => this.setState( { isLoading: false } ));
  }

  componentDidMount() {
    this.fetchSinks();
  }

  openEdit(title, sink, isEdit) {
    this.setState({editIsOpen: true, editTitle: title, sink: sink, isEdit: isEdit});
  }

  applyEdit() {
    this.closeEdit();
    this.fetchSinks();
  }

  onReloadClick() {
    this.fetchSinks();
  }

  closeEdit() {
    this.setState({editIsOpen: false});
    this.setState({removeIsOpen: false});
    this.fetchSinks();
  }

  onEditClick(id) {
    const sink = this.state.sinks.find(s => s.id === id);
    this.openEdit("Edit Sink " + id, sink, true);
  }

  onTrashClick(id) {
    const sink = this.state.sinks.find(s => s.id === id);
    this.setState({removeIsOpen: true, sink: sink});
  }

  onAddClick() {
    let id;
    /* find first free id */
    for (id = 0; id < 63; id++) {
      if (this.state.sinks[id] === undefined ||
          this.state.sinks[id].id !== id) {
        break;
      }
    }
    const defaultSink = {
      'id': id,
      'name': 'ALSA Sink ' + id,
      'io': 'Audio Device',
      'delay_ms': 12,
      'use_sdp': false,
      'source': RestAPI.getBaseUrl() + '/api/source/sdp/' + id,
      'sdp': '',
      'ignore_refclk_gmid': true,
      'stream': false,
      'map': [ (id * 2) % 64, (id * 2 + 1) % 64 ]
    };
    this.openEdit('Add Sink ' + id, defaultSink, false);
  }

  render() {
    this.state.sinks.sort((a, b) => (a.id > b.id) ? 1 : -1);
    const sinks = this.state.sinks.map((sink) => (
      <SinkEntry key={sink.id}
        id={sink.id}
        channels={sink.map.length}
        name={sink.name}
        attached={sink.attached !== false}
        detachReason={sink.detach_reason || ''}
        onEditClick={this.onEditClick}
        onTrashClick={this.onTrashClick}
      />
    ));
    return (
      <div id='sinks'>
       { this.state.isLoading ? <Loader/>
	   : <SinkList onAddClick={this.onAddClick}
               onReloadClick={this.onReloadClick}
               sinks={sinks} /> }
       { this.state.editIsOpen ?
        <SinkEdit editIsOpen={this.state.editIsOpen}
          closeEdit={this.closeEdit}
          applyEdit={this.applyEdit}
          editTitle={this.state.editTitle}
	  isEdit={this.state.isEdit}
	  sink={this.state.sink} />
           : undefined }
       { this.state.removeIsOpen ?
        <SinkRemove removeIsOpen={this.state.removeIsOpen}
          closeEdit={this.closeEdit}
          applyEdit={this.applyEdit}
	  sink={this.state.sink}
	  key={this.state.sink.id} />
           : undefined }
      </div>
    );
  }
}

export default Sinks;
