﻿#include "../../shogi.h"
#ifdef LOCAL_GAME_SERVER

#include "../../extra/all.h"

#include <windows.h>

// 子プロセスを実行して、子プロセスの標準入出力をリダイレクトするのをお手伝いするクラス。
struct ProcessNegotiator
{
  ProcessNegotiator() { init(); }

  // 子プロセスの実行
  void run(string app_path_)
  {
    wstring app_path = to_wstring(app_path_);

    ZeroMemory(&pi, sizeof(pi));
    ZeroMemory(&si, sizeof(si));

    si.cb = sizeof(si);
    si.hStdInput = child_std_in_read;
    si.hStdOutput = child_std_out_write;
    si.dwFlags |= STARTF_USESTDHANDLES;

    // Create the child process

    BOOL success = ::CreateProcess(app_path.c_str(), // ApplicationName
      NULL, // CmdLine
      NULL, // security attributes
      NULL, // primary thread security attributes
      TRUE, // handles are inherited
      0,    // creation flags
      NULL, // use parent's environment
      NULL, // use parent's current directory
      &si,  // STARTUPINFO pointer
      &pi   // receives PROCESS_INFOMATION
      );

    if (!success)
      sync_cout << "CreateProcessに失敗" << endl;
  }

  // 長手数になるかも知れないので…。
  static const int BUF_SIZE = 4096;

  string read()
  {
    auto result = read_next();
    if (!result.empty())
      return result;

    // ReadFileは同期的に使いたいが、しかしデータがないときにブロックされるのは困るので
    // pipeにデータがあるのかどうかを調べてからReadFile()する。

    DWORD dwRead , dwReadTotal , dwLeft;
    CHAR chBuf[BUF_SIZE];

    // bufferサイズは1文字少なく申告して終端に'\0'を付与してstring化する。

    BOOL success = ::PeekNamedPipe(
      child_std_out_read, // [in]  handle of named pipe
      chBuf,              // [out] buffer     
      BUF_SIZE-1,         // [in]  buffer size
      &dwRead,            // [out] bytes read
      &dwReadTotal,       // [out] total bytes avail
      &dwLeft             // [out] bytes left this message
      );

    if (success && dwReadTotal > 0)
    {
      success = ::ReadFile(child_std_out_read, chBuf, BUF_SIZE - 1, &dwRead, NULL);

      if (success && dwRead != 0)
      {
        chBuf[dwRead] = '\0'; // 終端マークを書いて文字列化する。
        read_buffer += string(chBuf);
      }
    }
    return read_next();
  }

  bool write(string str)
  {
    str += "\r\n"; // 改行コードの付与
    DWORD dwWritten;
    BOOL success = ::WriteFile(child_std_in_write, str.c_str(), DWORD(str.length()) , &dwWritten, NULL);
    return success;
  }

protected:

  void init()
  {
    // pipeの作成

    SECURITY_ATTRIBUTES saAttr;

    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;

#define ERROR_MES(mes) { sync_cout << mes << sync_endl; return ; }

    if (!::CreatePipe(&child_std_out_read, &child_std_out_write, &saAttr, 0))
      ERROR_MES("error CreatePipe : std out");

    if (!::SetHandleInformation(child_std_out_read, HANDLE_FLAG_INHERIT, 0))
      ERROR_MES("error SetHandleInformation : std out");
    
    if (!::CreatePipe(&child_std_in_read, &child_std_in_write, &saAttr, 0))
      ERROR_MES("error CreatePipe : std in");

    if (!::SetHandleInformation(child_std_in_write, HANDLE_FLAG_INHERIT, 0))
      ERROR_MES("error SetHandleInformation : std in");

#undef ERROR_MES
  }

  string read_next()
  {
    // read_bufferから改行までを切り出す
    auto it = read_buffer.find("\n");
    if (it == string::npos)
      return string();
    // 切り出したいのは"\n"の手前まで(改行コード不要)、このあと"\n"は捨てたいので
    // it+1から最後までが次回まわし。
    auto result = read_buffer.substr(0, it);
    read_buffer = read_buffer.substr(it+1, read_buffer.size() - it);
    // "\r\n"かも知れないので"\r"も除去。
    if (result.size() && result[result.size() - 1] == '\r')
      result = result.substr(0, result.size() - 1);
    return result;
  }

  // wstring変換
  wstring to_wstring(const string& src)
  {
    size_t ret;
    wchar_t *wcs = new wchar_t[src.length() + 1];
    ::mbstowcs_s(&ret,wcs, src.length()+1, src.c_str(), _TRUNCATE);
    wstring result = wcs;
    delete[] wcs;
    return result;
  }

  PROCESS_INFORMATION pi;
  STARTUPINFO si;

  HANDLE child_std_out_read;
  HANDLE child_std_out_write;
  HANDLE child_std_in_read;
  HANDLE child_std_in_write;

  // 受信バッファ
  string read_buffer;
};

struct EngineState
{
  void run(string path)
  {
    pn.run(path);
    state = START_UP;
  }

  void on_idle()
  {
    switch (state)
    {
    case START_UP:
      pn.write("usi");
      state = WAIT_USI_OK;
      break;

    case WAIT_USI_OK:
    {
      string line = pn.read();
      if (line == "usiok")
        state = IS_READY;
      else if (line.substr(0,min(line.size(),8)) == "id name ")
        engine_name_ = line.substr(8,line.size()-8);
      break;
    }

    case IS_READY:
      // エンジンの初期化コマンドを送ってやる
      for (auto line : engine_config)
        pn.write(line);

      pn.write("isready");
      state = WAIT_READY_OK;
      break;

    case WAIT_READY_OK:
      if (pn.read() == "readyok")
      {
        pn.write("usinewgame");
        state = GAME_START;
      }
      break;

    case GAME_START:
      break;

    case GAME_OVER:
      pn.write("gameover");
      state = START_UP;
      break;
    }

  }

  Move think(const Position& pos, const string& think_cmd)
  {
    string sfen;
    sfen = "position startpos moves " + pos.moves_from_start();
    pn.write(sfen);
    pn.write(think_cmd);
    string bestmove;
    while (true)
    {
      bestmove = pn.read();
      if (bestmove.find("bestmove") != string::npos)
        break;
    }
    istringstream is(bestmove);
    string token;
    is >> skipws >> token; // "bestmove"
    is >> token; // "7g7f" etc..

    Move m = move_from_usi(pos,token);
    if (m == MOVE_NONE)
    {
      sync_cout << "Error : bestmove = " << token << endl << pos << sync_endl;
    }
    return m;
  }

  enum State {
    START_UP, WAIT_USI_OK, IS_READY , WAIT_READY_OK, GAME_START, GAME_OVER,
  };

  // 対局の準備が出来たのか？
  bool is_game_started() const { return state == GAME_START; }

  // エンジンの初期化時に渡したいメッセージ
  void set_engine_config(vector<string>& lines) { engine_config = lines; }

  // ゲーム終了時に呼び出すべし。
  void game_over() { state = GAME_OVER; }

  // usiコマンドに対して思考エンジンが"is name ..."で返してきたengine名
  string engine_name() const { return engine_name_; }

protected:
  ProcessNegotiator pn;

  // 内部状態
  State state;

  // エンジン起動時に送信すべきコマンド
  vector<string> engine_config;

  // usiコマンドに対して思考エンジンが"is name ..."で返してきたengine名
  string engine_name_;

};

// --- Search

/*
  実行ファイルと同じフォルダに次の2つのファイルを配置。

  engine-config1.txt : 1つ目の思考エンジン
  engine-config2.txt : 2つ目の思考エンジン
  
    1行目にengineの実行ファイル名
    2行目に思考時のコマンド
    3行目以降にsetoption等、初期化時に思考エンジンに送りたいコマンドを書く。

  例)
    test.exe
    go btime 100 wtime 100 byoyomi 0
    setoption name Threads value 1
    setoption name Hash value 1024

  次に
  goコマンドを打つ。
  stopもしくはquitで終了するまで対局結果が出力される。

  O : engine1勝ち
  X : engine1負け
  . : 引き分け

  対局回数の指定)
  go btime [対局回数] wtime [予約1] byoyomi [予約2]
  　　　　　　↑ここに書くとその回数で停止

*/

void Search::init() {}
void Search::clear() {}
void MainThread::think() {
  
  fstream f[2];
  f[0].open("engine-config1.txt");
  f[1].open("engine-config2.txt");
  
  // 思考エンジンの実行ファイル名
  string engine_name[2];
  getline(f[0], engine_name[0]);
  getline(f[1], engine_name[1]);

  // 思考時のコマンド
  string think_cmd[2];
  getline(f[0], think_cmd[0]);
  getline(f[1], think_cmd[1]);

  EngineState es[2];
  es[0].run(engine_name[0]);
  es[1].run(engine_name[1]);

  for (int i = 0; i < 2; ++i) {
    vector<string> lines;
    string line;
    while (!f[i].eof())
    {
      getline(f[i], line);
      if (!line.empty())
        lines.push_back(line);
    }
    es[i].set_engine_config(lines);
  }

  uint64_t win = 0, draw = 0, lose = 0;
  Color player1_color = BLACK;

  bool game_started = false;

  // 対局回数。btimeの値がmax_games
  int max_games = Search::Limits.time[BLACK];
  if (max_games == 0)
    max_games = 100; // デフォルトでは100回
  int games = 0;

  // 対局開始時のハンドラ
  auto game_start = [&] {
    rootPos.set_hirate();
    game_started = true;
    Search::SetupStates = Search::StateStackPtr(new std::stack<StateInfo>);
  };

  // 対局終了時のハンドラ
  auto game_over = [&] {
    if (rootPos.game_ply() >= 256) // 長手数につき引き分け
    {
      draw++;
      cout << '.'; // 引き分けマーク
    }
    else if (rootPos.side_to_move() == player1_color)
    {
      lose++;
      cout << 'X'; // 負けマーク
    }
    else
    {
      win++;
      cout << 'O'; // 勝ちマーク
    }
    player1_color = ~player1_color; // 先後入れ替える。
//    sync_cout << rootPos << sync_endl; // デバッグ用に投了の局面を表示させてみる
    game_started = false;

    es[0].game_over();
    es[1].game_over();
    games++;
  };
  
  sync_cout << "local game server start : " << engine_name[0] << " vs " << engine_name[1] << sync_endl;

  string line;
  while (!Search::Signals.stop && games < max_games)
  {
    es[0].on_idle();
    es[1].on_idle();

    if (!game_started && es[0].is_game_started() && es[1].is_game_started())
    {
      game_start();
      //sync_cout << "game start" << sync_endl;
    }

    // ゲーム中であれば局面を送って思考させる
    if (game_started)
    {
      int player = (rootPos.side_to_move() == player1_color) ? 0 : 1;
      Move m = es[player].think(rootPos,think_cmd[player]);

      if (m != MOVE_RESIGN)
      {
        Search::SetupStates->push(StateInfo());
        rootPos.do_move(m, Search::SetupStates->top());
      }

      if (m == MOVE_RESIGN || rootPos.is_mated() || rootPos.game_ply() >= 256)
      {
        game_over();
        //sync_cout << "game over" << sync_endl;
      }
    }

  }

  sync_cout << endl << "local game server end : [" << es[0].engine_name() << "] vs [" << es[1].engine_name() << "]" << sync_endl;
  sync_cout << "GameResult " << win << " - " << draw << " - " << lose << sync_endl;
}
void Thread::search() {}

#endif
