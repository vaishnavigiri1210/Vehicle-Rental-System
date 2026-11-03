
#include "httplib.h"
#include "json.hpp"
#include <sqlite3.h>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>

using json = nlohmann::json;
using namespace std;

class Vehicle {
protected:
    int id;
    string vehicleType, brand, model, fuel, transmission, image, description;
    int seats;
    double ratePerDay;
    bool available;

public:
    Vehicle(int i, string t, string b, string m, double r, bool a, int s,
            string f, string tr, string img, string desc)
        : id(i), vehicleType(move(t)), brand(move(b)), model(move(m)),
          fuel(move(f)), transmission(move(tr)), image(move(img)),
          description(move(desc)), seats(s), ratePerDay(r), available(a) {}

    virtual ~Vehicle() = default;
    virtual double calculateRent(int days) const = 0;
    virtual string type() const { return vehicleType; }

    double getRate() const { return ratePerDay; }
    bool isAvailable() const { return available; }
    void setAvailable(bool a) { available = a; }

    json toJson() const {
        return {
            {"id",id},{"type",type()},{"brand",brand},{"model",model},
            {"ratePerDay",ratePerDay},{"available",available},{"seats",seats},
            {"fuel",fuel},{"transmission",transmission},{"image",image},
            {"description",description}
        };
    }
};

class Car : public Vehicle {
public:
    using Vehicle::Vehicle;
    double calculateRent(int days) const override {
        return ratePerDay * days + 500;
    }
};

class TwoWheeler : public Vehicle {
public:
    using Vehicle::Vehicle;
    double calculateRent(int days) const override {
        return ratePerDay * days;
    }
};

class OtherVehicle : public Vehicle {
public:
    using Vehicle::Vehicle;
    double calculateRent(int days) const override {
        return ratePerDay * days;
    }
};

class Database {
    sqlite3* db = nullptr;

    bool hasColumn(const string& table, const string& column) {
        string sql = "PRAGMA table_info(" + table + ")";
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr);

        bool found = false;
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char* name = sqlite3_column_text(st, 1);
            if (name && column == reinterpret_cast<const char*>(name))
                found = true;
        }

        sqlite3_finalize(st);
        return found;
    }

public:
    explicit Database(const string& path) {
        if (sqlite3_open(path.c_str(), &db) != SQLITE_OK)
            throw runtime_error("Cannot open database file");

        exec("PRAGMA foreign_keys=ON;"
             "CREATE TABLE IF NOT EXISTS vehicles(id INTEGER PRIMARY KEY AUTOINCREMENT,type TEXT NOT NULL,brand TEXT NOT NULL,model TEXT NOT NULL,rate REAL NOT NULL,available INTEGER NOT NULL DEFAULT 1);"
             "CREATE TABLE IF NOT EXISTS customers(id INTEGER PRIMARY KEY AUTOINCREMENT,name TEXT NOT NULL,phone TEXT NOT NULL,license TEXT NOT NULL UNIQUE);"
             "CREATE TABLE IF NOT EXISTS bookings(id INTEGER PRIMARY KEY AUTOINCREMENT,vehicle_id INTEGER NOT NULL REFERENCES vehicles(id),customer_id INTEGER NOT NULL REFERENCES customers(id),days INTEGER NOT NULL,amount REAL NOT NULL,late_fee REAL,active INTEGER NOT NULL DEFAULT 1);");

        addColumn("vehicles","seats","INTEGER NOT NULL DEFAULT 4");
        addColumn("vehicles","fuel","TEXT NOT NULL DEFAULT 'Petrol'");
        addColumn("vehicles","transmission","TEXT NOT NULL DEFAULT 'Manual'");
        addColumn("vehicles","image","TEXT NOT NULL DEFAULT ''");
        addColumn("vehicles","description","TEXT NOT NULL DEFAULT ''");
        addColumn("customers","email","TEXT NOT NULL DEFAULT ''");
        addColumn("customers","address","TEXT NOT NULL DEFAULT ''");
    }

    ~Database() {
        if (db) sqlite3_close(db);
    }

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    sqlite3* raw() const { return db; }
    long long lastId() const { return sqlite3_last_insert_rowid(db); }

    void exec(const string& sql) {
        char* err = nullptr;

        if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
            string m = err ? err : "Database error";
            sqlite3_free(err);
            throw runtime_error(m);
        }
    }

    void addColumn(const string& table, const string& col,
                   const string& definition) {
        if (!hasColumn(table, col))
            exec("ALTER TABLE " + table + " ADD COLUMN " + col + " " + definition);
    }
};

class Stmt {
    sqlite3* db;
    sqlite3_stmt* s = nullptr;

public:
    Stmt(sqlite3* d, const string& sql) : db(d) {
        if (sqlite3_prepare_v2(d, sql.c_str(), -1, &s, nullptr) != SQLITE_OK)
            throw runtime_error(sqlite3_errmsg(d));
    }

    ~Stmt() { sqlite3_finalize(s); }

    Stmt& bind(int i, int v) {
        sqlite3_bind_int(s, i, v);
        return *this;
    }

    Stmt& bind(int i, double v) {
        sqlite3_bind_double(s, i, v);
        return *this;
    }

    Stmt& bind(int i, const string& v) {
        sqlite3_bind_text(s, i, v.c_str(), -1, SQLITE_TRANSIENT);
        return *this;
    }

    bool step() {
        int rc = sqlite3_step(s);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw runtime_error(sqlite3_errmsg(db));
    }

    int getInt(int c) const { return sqlite3_column_int(s, c); }
    double getDouble(int c) const { return sqlite3_column_double(s, c); }

    bool isNull(int c) const {
        return sqlite3_column_type(s, c) == SQLITE_NULL;
    }

    string getText(int c) const {
        auto t = sqlite3_column_text(s, c);
        return t ? reinterpret_cast<const char*>(t) : "";
    }
};

class RentalManager {
    Database& db;
    unordered_map<int,shared_ptr<Vehicle>> vehicles;
    mutable mutex mu;

    static shared_ptr<Vehicle> make(
        const string&t, int id, const string&b, const string&m,
        double r, bool a, int seats, const string&fuel,
        const string&trans, const string&img, const string&desc) {

        if (t=="Car" || t=="SUV" || t=="MUV" ||
            t=="Van" || t=="Tempo Traveller")
            return make_shared<Car>(id,t,b,m,r,a,seats,fuel,trans,img,desc);

        if (t=="Bike" || t=="Scooter")
            return make_shared<TwoWheeler>(id,t,b,m,r,a,seats,fuel,trans,img,desc);

        return make_shared<OtherVehicle>(
            id,t,b,m,r,a,seats,fuel,trans,img,desc);
    }

    void inTransaction(const function<void()>& fn) {
        db.exec("BEGIN");
        try {
            fn();
            db.exec("COMMIT");
        }
        catch (...) {
            db.exec("ROLLBACK");
            throw;
        }
    }

public:
    explicit RentalManager(Database& d) : db(d) {
        Stmt s(db.raw(),
            "SELECT id,type,brand,model,rate,available,seats,fuel,transmission,image,description FROM vehicles ORDER BY id");

        while (s.step()) {
            vehicles[s.getInt(0)] = make(
                s.getText(1),s.getInt(0),s.getText(2),s.getText(3),
                s.getDouble(4),s.getInt(5)==1,s.getInt(6),
                s.getText(7),s.getText(8),s.getText(9),s.getText(10));
        }

        seed();
    }

    void seed() {
        struct V {
            string t,b,m;
            double r;
            int s;
            string f,tr,img,d;
        };

        vector<V> samples = {
            {"Bike","Royal Enfield","Classic 350",1200,2,"Petrol","Manual","https://images.unsplash.com/photo-1558981806-ec527fa84c39?w=900","Classic road bike"},
            {"Scooter","Honda","Activa 6G",450,2,"Petrol","Automatic","https://images.unsplash.com/photo-1558981403-c5f9899a28bc?w=900","Easy city scooter"},
            {"Bike","Yamaha","FZ",700,2,"Petrol","Manual","https://images.unsplash.com/photo-1558981806-ec527fa84c39?w=900","Street motorcycle"},
            {"Car","Maruti Suzuki","Swift",1500,5,"Petrol","Manual","https://images.unsplash.com/photo-1492144534655-ae79c964c9d7?w=900","Compact city hatchback"},
            {"Car","Hyundai","i20",1800,5,"Petrol","Automatic","https://images.unsplash.com/photo-1503376780353-7e6692767b70?w=900","Premium hatchback"},
            {"Car","Honda","City",2200,5,"Petrol","Automatic","https://images.unsplash.com/photo-1552519507-da3b142c6e3d?w=900","Comfortable family sedan"},
            {"SUV","Mahindra","Scorpio N",3200,7,"Diesel","Manual","https://images.unsplash.com/photo-1519641471654-76ce0107ad1b?w=900","7-seater adventure SUV"},
            {"SUV","Toyota","Innova Crysta",3800,7,"Diesel","Manual","https://images.unsplash.com/photo-1511919884226-fd3cad34687c?w=900","Spacious family MUV"},
            {"Tempo Traveller","Force","Urbania",6500,12,"Diesel","Manual","https://images.unsplash.com/photo-1549317661-bd32c8ce0db2?w=900","Group travel vehicle"}
        };

        for (auto& v : samples) {
            bool exists = false;
            {
                Stmt check(db.raw(),
                    "SELECT 1 FROM vehicles WHERE brand=? AND model=? LIMIT 1");
                check.bind(1,v.b).bind(2,v.m);
                exists = check.step();
            }

            if (!exists)
                addVehicle(v.t,v.b,v.m,v.r,v.s,v.f,v.tr,v.img,v.d);
        }
    }

    int addVehicle(const string&t,const string&b,const string&m,double r,
                   int seats=4,const string&fuel="Petrol",
                   const string&trans="Manual",const string&img="",
                   const string&desc="") {
        lock_guard<mutex> lk(mu);

        if (t.empty() || b.empty() || m.empty() || r<=0 || seats<1)
            throw runtime_error("Enter valid vehicle details and a rate above zero.");

        Stmt s(db.raw(),
            "INSERT INTO vehicles(type,brand,model,rate,available,seats,fuel,transmission,image,description) VALUES(?,?,?,?,1,?,?,?,?,?)");

        s.bind(1,t).bind(2,b).bind(3,m).bind(4,r).bind(5,seats)
         .bind(6,fuel).bind(7,trans).bind(8,img).bind(9,desc);

        s.step();

        int id = (int)db.lastId();
        vehicles[id] = make(t,id,b,m,r,true,seats,fuel,trans,img,desc);
        return id;
    }

    json listVehicles() const {
        lock_guard<mutex> lk(mu);
        json a = json::array();
        for (auto& p : vehicles) a.push_back(p.second->toJson());
        return a;
    }

    int addCustomer(const string&n,const string&p,const string&l,
                    const string&e,const string&a) {
        lock_guard<mutex> lk(mu);

        if (n.empty() || p.empty() || l.empty())
            throw runtime_error("Name, phone and licence number are required.");

        try {
            Stmt s(db.raw(),
                "INSERT INTO customers(name,phone,license,email,address) VALUES(?,?,?,?,?)");
            s.bind(1,n).bind(2,p).bind(3,l).bind(4,e).bind(5,a);
            s.step();
        }
        catch (const runtime_error& x) {
            if (string(x.what()).find("UNIQUE") != string::npos)
                throw runtime_error("This licence number is already registered.");
            throw;
        }

        return (int)db.lastId();
    }

    json listCustomers() const {
        lock_guard<mutex> lk(mu);
        json a = json::array();

        Stmt s(db.raw(),
            "SELECT id,name,phone,license,email,address FROM customers ORDER BY id");

        while (s.step()) {
            a.push_back({
                {"id",s.getInt(0)},{"name",s.getText(1)},
                {"phone",s.getText(2)},{"license",s.getText(3)},
                {"email",s.getText(4)},{"address",s.getText(5)}
            });
        }

        return a;
    }

    // NEW: Delete customer safely
    void deleteCustomer(int id) {
        lock_guard<mutex> lk(mu);

        Stmt check(db.raw(),
            "SELECT COUNT(*) FROM bookings WHERE customer_id=?");
        check.bind(1,id);
        check.step();

        if (check.getInt(0) > 0) {
            throw runtime_error(
                "Cannot delete this customer because booking history exists.");
        }

        Stmt s(db.raw(), "DELETE FROM customers WHERE id=?");
        s.bind(1,id);
        s.step();

        if (sqlite3_changes(db.raw()) == 0) {
            throw runtime_error("Customer not found.");
        }
    }

    json book(int vid,int cid,int days) {
        lock_guard<mutex> lk(mu);

        if (days<1) throw runtime_error("Days must be at least 1.");

        auto it = vehicles.find(vid);
        if (it==vehicles.end()) throw runtime_error("Vehicle not found.");
        if (!it->second->isAvailable())
            throw runtime_error("Vehicle is already booked.");

        {
            Stmt c(db.raw(),"SELECT 1 FROM customers WHERE id=?");
            c.bind(1,cid);
            if (!c.step()) throw runtime_error("Customer not found.");
        }

        double amount = it->second->calculateRent(days);
        int bid = 0;

        inTransaction([&] {
            Stmt b(db.raw(),
                "INSERT INTO bookings(vehicle_id,customer_id,days,amount) VALUES(?,?,?,?)");
            b.bind(1,vid).bind(2,cid).bind(3,days).bind(4,amount);
            b.step();
            bid = (int)db.lastId();

            Stmt v(db.raw(),"UPDATE vehicles SET available=0 WHERE id=?");
            v.bind(1,vid);
            v.step();
        });

        it->second->setAvailable(false);
        return {{"bookingId",bid},{"amount",amount}};
    }

    json returnVehicle(int bid,int extra) {
        lock_guard<mutex> lk(mu);

        if (extra<0) throw runtime_error("Extra days cannot be negative.");

        int vid=0;
        double amount=0;

        {
            Stmt s(db.raw(),
                "SELECT vehicle_id,amount FROM bookings WHERE id=? AND active=1");
            s.bind(1,bid);
            if (!s.step())
                throw runtime_error("Invalid or already returned booking.");
            vid=s.getInt(0);
            amount=s.getDouble(1);
        }

        auto it=vehicles.find(vid);
        double rate=it==vehicles.end()?0:it->second->getRate();
        double late=extra*rate*1.5;

        inTransaction([&] {
            Stmt b(db.raw(),
                "UPDATE bookings SET active=0,late_fee=? WHERE id=?");
            b.bind(1,late).bind(2,bid);
            b.step();

            Stmt v(db.raw(),"UPDATE vehicles SET available=1 WHERE id=?");
            v.bind(1,vid);
            v.step();
        });

        if (it!=vehicles.end()) it->second->setAvailable(true);

        return {{"totalPaid",amount+late},{"lateFee",late}};
    }

    json listBookings() const {
        lock_guard<mutex> lk(mu);
        json a=json::array();

        Stmt s(db.raw(),
            "SELECT b.id,c.name,v.brand,v.model,b.days,b.amount,b.late_fee,b.active FROM bookings b JOIN customers c ON c.id=b.customer_id JOIN vehicles v ON v.id=b.vehicle_id ORDER BY b.id DESC");

        while (s.step()) {
            json x = {
                {"bookingId",s.getInt(0)},
                {"customer",s.getText(1)},
                {"vehicle",s.getText(2)+" "+s.getText(3)},
                {"days",s.getInt(4)},
                {"amount",s.getDouble(5)},
                {"active",s.getInt(7)==1}
            };

            if (s.isNull(6)) {
                x["lateFee"]=nullptr;
                x["totalPaid"]=nullptr;
            } else {
                x["lateFee"]=s.getDouble(6);
                x["totalPaid"]=s.getDouble(5)+s.getDouble(6);
            }

            a.push_back(x);
        }

        return a;
    }
};

int main() {
    try {
        Database db("rental.db");
        RentalManager mgr(db);
        httplib::Server svr;

        svr.set_mount_point("/","../frontend");

        auto handle=[](function<json(const json&)> fn) {
            return [fn](const httplib::Request& req,httplib::Response& res) {
                try {
                    json j=req.body.empty()?json::object():json::parse(req.body);
                    res.set_content(fn(j).dump(),"application/json");
                }
                catch (const exception& e) {
                    res.status=400;
                    res.set_content(json{{"error",e.what()}}.dump(),
                                    "application/json");
                }
            };
        };

        svr.Get("/api/vehicles",
            handle([&](const json&) { return mgr.listVehicles(); }));

        svr.Post("/api/vehicles",
            handle([&](const json& j) {
                return json{{"id",mgr.addVehicle(
                    j.at("type").get<string>(),
                    j.at("brand").get<string>(),
                    j.at("model").get<string>(),
                    j.at("ratePerDay").get<double>(),
                    j.value("seats",4),
                    j.value("fuel",string("Petrol")),
                    j.value("transmission",string("Manual")),
                    j.value("image",string("")),
                    j.value("description",string(""))
                )}};
            }));

        svr.Get("/api/customers",
            handle([&](const json&) { return mgr.listCustomers(); }));

        svr.Post("/api/customers",
            handle([&](const json& j) {
                return json{{"id",mgr.addCustomer(
                    j.at("name").get<string>(),
                    j.at("phone").get<string>(),
                    j.at("license").get<string>(),
                    j.value("email",string("")),
                    j.value("address",string(""))
                )}};
            }));

        // NEW: Delete customer API
        svr.Delete(R"(/api/customers/(\d+))",
            [&](const httplib::Request& req, httplib::Response& res) {
                try {
                    int id = stoi(req.matches[1]);
                    mgr.deleteCustomer(id);
                    res.set_content(
                        json{{"success",true}}.dump(),
                        "application/json"
                    );
                }
                catch (const exception& e) {
                    res.status=400;
                    res.set_content(
                        json{{"error",e.what()}}.dump(),
                        "application/json"
                    );
                }
            });

        svr.Post("/api/book",
            handle([&](const json& j) {
                return mgr.book(
                    j.at("vehicleId").get<int>(),
                    j.at("customerId").get<int>(),
                    j.at("days").get<int>()
                );
            }));

        svr.Post("/api/return",
            handle([&](const json& j) {
                return mgr.returnVehicle(
                    j.at("bookingId").get<int>(),
                    j.at("extraDays").get<int>()
                );
            }));

        svr.Get("/api/bookings",
            handle([&](const json&) { return mgr.listBookings(); }));

        cout << "FleetDesk running at http://localhost:8080" << endl;

        if (!svr.listen("0.0.0.0",8080)) {
            cerr << "Could not start server. Check whether port 8080 is already in use." << endl;
            return 1;
        }
    }
    catch (const exception& e) {
        cerr << "Fatal error: " << e.what() << endl;
        return 1;
    }

    return 0;
}
