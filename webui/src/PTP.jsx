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

class PTPDomainStatus extends Component {
  static propTypes = {
    domain: PropTypes.number.isRequired,
    status: PropTypes.string.isRequired,
    gmid: PropTypes.string.isRequired,
    jitter: PropTypes.number.isRequired,
  };

  render() {
    const color = this.props.status === 'locked' ? '#2a0'
                : this.props.status === 'locking' ? '#d90' : '#c00';
    // no grandmaster when unlocked -> don't show a stale/zeroed GMID.
    const gmid = this.props.status === 'unlocked' ? '—' : this.props.gmid;
    return (
     <div style={{marginBottom: '1em'}}>
      <h3>Domain {this.props.domain}&nbsp;&nbsp;
        <span style={{color: color}}>&#9679;</span>&nbsp;
        <span style={{color: color}}>{this.props.status}</span>
      </h3>
      <table><tbody>
        <tr>
          <th align="left"> <label>GMID</label> </th>
          <th align="left"> <input size="30" value={gmid} disabled/> </th>
        </tr>
        <tr>
          <th align="left"> <label>Clock jitter</label> </th>
          <th align="left"> <input value={this.props.jitter} disabled/> </th>
        </tr>
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
              <PTPDomainStatus key={d.domain} domain={d.domain}
                status={d.status} gmid={d.gmid} jitter={parseInt(d.jitter, 10)}/>) }
      </div>
    )
  }
}

export default PTP;
