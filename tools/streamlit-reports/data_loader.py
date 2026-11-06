"""
Data loader utility for grinder analysis reports
"""
import sqlite3
import pandas as pd
import os
from typing import Optional, Dict, Any
from flow_analysis import calculate_flow_rate_stats, calculate_grind_efficiency

class GrindDataLoader:
    def __init__(self, db_path: str = None):
        if db_path is None:
            # Check for environment variable first, then fall back to default
            db_path = os.environ.get('GRIND_DB_PATH', '../database/grinder_data.db')
        self.db_path = db_path
        self.profile_map = {0: "SINGLE", 1: "DOUBLE", 2: "CUSTOM", 3: "PROFILE"}
    
    def _get_connection(self):
        """Get database connection"""
        if not os.path.exists(self.db_path):
            raise FileNotFoundError(f"Database file '{self.db_path}' not found")
        return sqlite3.connect(self.db_path)
    
    def get_sessions(self) -> pd.DataFrame:
        """Load all grind sessions"""
        with self._get_connection() as conn:
            sessions = pd.read_sql_query("SELECT * FROM grind_sessions", conn)
            sessions['profile_name'] = sessions['profile_id'].map(self.profile_map)
            # Convert timestamp column - handle both session_timestamp and timestamp
            if 'session_timestamp' in sessions.columns:
                sessions['timestamp'] = pd.to_datetime(sessions['session_timestamp'], unit='s')
            elif 'timestamp' in sessions.columns:
                sessions['timestamp'] = pd.to_datetime(sessions['timestamp'])
            # Convert time from milliseconds to seconds
            if 'total_time_ms' in sessions.columns:
                sessions['total_time_s'] = sessions['total_time_ms'] / 1000.0
            return sessions
    
    def get_events(self, session_id: Optional[int] = None) -> pd.DataFrame:
        """Load grind events, optionally filtered by session"""
        with self._get_connection() as conn:
            query = "SELECT * FROM grind_events"
            params = ()
            if session_id:
                query += " WHERE session_id = ?"
                params = (session_id,)
            events = pd.read_sql_query(query, conn, params=params)
            # Events table already has timestamp_ms, no conversion needed
            return events
    
    def get_measurements(self, session_id: Optional[int] = None) -> pd.DataFrame:
        """Load grind measurements, optionally filtered by session"""
        with self._get_connection() as conn:
            query = "SELECT * FROM grind_measurements"
            params = ()
            if session_id:
                query += " WHERE session_id = ?"
                params = (session_id,)
            measurements = pd.read_sql_query(query, conn, params=params)
            return measurements
    
    def get_session_summary(self, session_id: int) -> Dict[str, Any]:
        """Get comprehensive session summary"""
        sessions = self.get_sessions()
        session = sessions[sessions['session_id'] == session_id].iloc[0]
        
        events = self.get_events(session_id)
        measurements = self.get_measurements(session_id)
        
        return {
            'session': session.to_dict(),
            'events': events,
            'measurements': measurements,
            'event_count': len(events),
            'measurement_count': len(measurements)
        }
    
    def get_available_sessions(self) -> pd.DataFrame:
        """Get summary of available sessions for selection"""
        sessions = self.get_sessions()
        return sessions[['session_id', 'timestamp', 'profile_name', 'target_weight', 'final_weight', 'error_grams', 'total_time_s']]
    
    def get_session_measurements(self, session_id: int) -> pd.DataFrame:
        """Get measurements for a specific session with time normalization"""
        measurements = self.get_measurements(session_id)
        if not measurements.empty and 'timestamp_ms' in measurements.columns:
            # Normalize timestamps to start from 0
            min_timestamp = measurements['timestamp_ms'].min()
            measurements['timestamp_s'] = (measurements['timestamp_ms'] - min_timestamp) / 1000.0
        return measurements
    
    def calculate_flow_rate_stats(self, session_id: int) -> Dict:
        """Calculate flow rate statistics for a session"""
        measurements = self.get_session_measurements(session_id)
        return calculate_flow_rate_stats(measurements)
    
    def calculate_session_efficiency(self, session_id: int) -> Dict:
        """Calculate efficiency metrics for a session"""
        sessions = self.get_sessions()
        session = sessions[sessions['session_id'] == session_id].iloc[0].to_dict()
        measurements = self.get_session_measurements(session_id)
        return calculate_grind_efficiency(session, measurements)