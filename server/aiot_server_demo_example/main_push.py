"""
详细可参考https://doc.shengwang.cn/doc/toybox/iot/get-started/run-example

"""
import base64
import uuid
import requests
import json
import logging
import random
import subprocess
import threading
import os
from RtcTokenBuilder2 import RtcTokenBuilder, Role_Publisher
from http.server import BaseHTTPRequestHandler, HTTPServer

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)

def load_config():
    """Load config.json file"""
    config_path = os.path.join(os.path.dirname(__file__), 'config.json')
    try:
        with open(config_path, 'r', encoding='utf-8') as f:
            config = json.load(f)
            logger.info('Successfully loaded config file')
            return config
    except Exception as e:
        logger.error(f'Failed to load config file: {str(e)}')
        raise

def run_pulling_program(executable_path, channel_name, token):
    """
    在一个新的子进程中运行拉流程序，并将 channel_name 和 token 作为命令行参数传递。
    """
    try:
        # 构建命令行参数
        cmd = [
            executable_path,
            '--channelId', channel_name,
            '--token', token,
            '--rtmpUrl','rtmps://a.rtmps.youtube.com/live2/8uhx-43v4-26my-z2eq-bat6'
        ]
        logger.info(f"准备启动拉流程序: {' '.join(cmd)}")

        # 使用 Popen 在新进程中运行拉流程序，这不会阻塞当前线程。
        process = subprocess.Popen(cmd)
        logger.info(f"已成功启动 C++ 程序，进程 PID: {process.pid}")
    except FileNotFoundError:
        logger.error(f"错误：拉流程序可执行文件未找到，路径: {executable_path}。请检查路径是否正确。")
    except Exception as e:
        logger.error(f"启动 C++ 程序时发生未知错误: {e}")


class RequestHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        """Handle POST requests"""
        if self.path == '/device':
            self.handle_device_request()
        elif self.path == '/agent/start':
            self.handle_agent_start_request()
        elif self.path == '/agent/stop':
            self.handle_agent_stop_request()
        else:
            self.send_response(404)
            self.end_headers()

    def handle_device_request(self):
        """Handle requests to /device endpoint"""
        content_length = int(self.headers['Content-Length'])
        post_data = self.rfile.read(content_length)
        
        try:
            data = json.loads(post_data)
            channel_name = data.get('channel_name')
            if not channel_name:
                logger.warning('Missing channel_name parameter')
                self.send_error_response(400, {'error': 'Missing channel_name parameter'})
                return

            uid = data.get('uid')
            if not uid:
                logger.warning('Missing uid parameter')
                self.send_error_response(400, {'error': 'Missing uid parameter'})
                return
                
            config = load_config()
            
            token = RtcTokenBuilder.build_token_with_uid(
                    config['app_id'],
                    config['app_certificate'],
                    channel_name,
                    uid,
                    Role_Publisher,
                    24 * 3600,  # token_expire
                    24 * 3600   # privilege_expire
                )
            
            if not token:
                logger.error('Failed to generate token')
                self.send_error_response(500, {
                    'error': 'Failed to generate token',
                    'reason': 'Please check if app_id and app_certificate in config.json are correct'
                })
                return
                
            # --- 为指定的拉流端额外生成 Token ---
            pull_uid = 999  # 您可以指定任何一个固定的、用于拉流的 UID
            logger.info(f'Generating a separate token for puller with uid: {pull_uid}')
            
            # 使用相同的频道名和配置，为 pull_uid 生成 Token
            # 注意：如果拉流端只需要订阅（拉流），更安全的做法是使用 Role_Subscriber
            pull_token = RtcTokenBuilder.build_token_with_uid(
                    config['app_id'],
                    config['app_certificate'],
                    channel_name,
                    pull_uid,
                    Role_Publisher, # 或者 Role_Subscriber
                    24 * 3600,
                    24 * 3600
                )

            if pull_token:
                logger.info(f'>>> Pull token for uid {pull_uid}: {pull_token}')
            else:
                logger.error(f'Failed to generate token for puller (uid: {pull_uid})')

            response = {
                'app_id': config['app_id'],
                'token': token
            }
            
            logger.info(f'Successfully generated token: {token[:10]}...')
            self.send_success_response(response)
            
        except Exception as e:
            logger.error(f'Error occurred while handling device request: {str(e)}')
            self.send_error_response(500, {'error': f'Server error: {str(e)}'})

    def handle_agent_start_request(self):
        """Handle requests to /agent/start endpoint"""
        content_length = int(self.headers['Content-Length'])
        post_data = self.rfile.read(content_length)
        
        try:
            data = json.loads(post_data)
            remote_uid = data.get('uid')
            agent_uid = data.get('agent_uid')
            
            if not remote_uid or not agent_uid:
                logger.warning('Missing remote_uid or agent_uid parameter')
                self.send_error_response(400, {'error': 'Missing remote_uid or agent_uid parameter'})
                return
                
            config = load_config()
            url = f"https://api.agora.io/api/conversational-ai-agent/v2/projects/{config['app_id']}/join"
            
            # Generate Basic Auth
            auth_str = f"{config['customer_key']}:{config['customer_secret']}"
            auth_bytes = auth_str.encode('ascii')
            base64_auth = base64.b64encode(auth_bytes).decode('ascii')
            
            headers = {
                'Content-Type': 'application/json',
                'Authorization': f"Basic {base64_auth}"
            }
            
            channel_name = data.get('channel_name')
            if not channel_name:
                logger.warning('Missing channel_name parameter')
                self.send_error_response(400, {'error': 'Missing channel_name parameter'})
                return
                
            token = RtcTokenBuilder.build_token_with_uid(
                config['app_id'],
                config['app_certificate'],
                channel_name,
                agent_uid,
                Role_Publisher,
                24 * 3600,  # token_expire
                24 * 3600   # privilege_expire
            )
            
            payload = {
                "name": str(uuid.uuid4()),
                "properties": {
                    "token": token,
                    "channel": channel_name,
                    "asr": config.get('asr', {}),
                    "parameters": config.get('parameters', {}),
                    "tts": config.get('tts', {}),
                    "agent_rtc_uid": str(agent_uid),
                    "remote_rtc_uids": [
                        str(remote_uid)
                    ],
                    "enable_string_uid": False,
                    "idle_timeout": config.get('idle_timeout', 30),
                    "advanced_features": {
                        "enable_aivad": False,
                        "enable_bhvs": True
                    },
                    "llm": config.get('llm', {})
                }
            }
            
            logger.info(f'Preparing to call Agora API, payload: {json.dumps(payload, indent=2)}')
            try:
                api_response = requests.post(url, headers=headers, json=payload)
                api_response.raise_for_status()
                
                response_data = api_response.json()
                logger.info(f'Agora API response: {json.dumps(response_data, indent=2)}')
                self.send_success_response(response_data)
                logger.info('Agora API call successful')
                
            except requests.exceptions.RequestException as e:
                logger.error(f'Failed to call Agora API: {str(e)}')
                self.send_error_response(500, {
                    'error': 'Failed to call Agora API',
                    'reason': str(e)
                })
                
        except Exception as e:
            logger.error(f'Error occurred while handling agent/start request: {str(e)}')
            self.send_error_response(500, {'error': f'Server error: {str(e)}'})

    def handle_agent_stop_request(self):
        """Handle requests to /agent/stop endpoint"""
        content_length = int(self.headers['Content-Length'])
        post_data = self.rfile.read(content_length)
        
        try:
            data = json.loads(post_data)
            agent_id = data.get('agent_id')
            
            if not agent_id:
                logger.warning('Missing agent_id parameter')
                self.send_error_response(400, {'error': 'Missing agent_id parameter'})
                return
        
            config = load_config()
            url = f"https://api.agora.io/api/conversational-ai-agent/v2/projects/{config['app_id']}/agents/{agent_id}/leave"
            
            # Generate Basic Auth
            auth_str = f"{config['customer_key']}:{config['customer_secret']}"
            auth_bytes = auth_str.encode('ascii')
            base64_auth = base64.b64encode(auth_bytes).decode('ascii')
            
            headers = {
                'Content-Type': 'application/json',
                'Authorization': f"Basic {base64_auth}"
            }
            
            logger.info(f'Preparing to call Agora leave API for agent {agent_id}')
            api_response = requests.post(url, headers=headers)
            self.send_response(api_response.status_code)
            self.send_header('Content-type', 'application/json')
            self.end_headers()
            self.wfile.write(json.dumps(api_response.json()).encode())
            logger.info(f'Processed leave request for agent {agent_id} with status {api_response.status_code}')
                
        except Exception as e:
            logger.error(f'Error occurred while handling agent/leave request: {str(e)}')
            self.send_error_response(500, {'error': f'Server error: {str(e)}'})    

    def send_success_response(self, data):
        """Send success response"""
        self.send_response(200)
        self.send_header('Content-type', 'application/json')
        self.end_headers()
        self.wfile.write(json.dumps(data).encode())

    def send_error_response(self, status_code, error_data):
        """Send error response"""
        self.send_response(status_code)
        self.send_header('Content-type', 'application/json')
        self.end_headers()
        self.wfile.write(json.dumps(error_data).encode())

def run(server_class=HTTPServer, handler_class=RequestHandler, port=5001):
    """Start HTTP server"""
    server_address = ('', port)
    httpd = server_class(server_address, handler_class)
    logger.info(f'Starting server on port {port}...')
    httpd.serve_forever()

if __name__ == '__main__':
    run()
